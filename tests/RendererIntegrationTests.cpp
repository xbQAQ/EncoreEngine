#include "Capture/CaptureService.h"
// 帧级集成从统一宿主驱动，资源类型通过其正式依赖组合。
#include "Frame/FrameRenderer.h"
#include "asset/GltfImporter.h"
#include "dx12/Dx12Device.h"
#include "dx12/Dx12Swapchain.h"
#include "dx12/GraphExecutor.h"
#include "dx12/PassParameters.h"
#include "dx12/ResourcePool.h"
#include "gameplay/World.h"
#include "editor/EditorHistory.h"
#include "render/RenderSettings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef RENDER_SOURCE_DIRECTORY
#define RENDER_SOURCE_DIRECTORY L"."
#endif

namespace {
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Image {
    unsigned width{};                          // 读回像素宽度，不包含 D3D12 布局填充。
    unsigned height{};                         // 读回像素高度。
    std::vector<std::array<double, 3>> pixels; // 按源格式保存显示线性 RGB 或原始场景线性 RGB。

    std::array<double, 3> Mean(unsigned firstRow = 0) const {
        std::array<double, 3> result{};
        for (unsigned y = firstRow; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                for (unsigned channel = 0; channel < 3; ++channel)
                    result[channel] += pixels[size_t(y) * width + x][channel];
        for (auto &value : result)
            value /= double(width) * (height - firstRow);
        return result;
    }
    double LitFraction() const {
        size_t lit = 0;
        for (const auto &pixel : pixels)
            if (*std::max_element(pixel.begin(), pixel.end()) > 0.002)
                ++lit;
        return double(lit) / pixels.size();
    }
};

// 比较解码像素，避免 PNG 元数据或蒙特卡洛帧种子变化干扰。
double Difference(const Image &a, const Image &b, unsigned firstRow = 0) {
    Require(a.width == b.width && a.height == b.height, "Compared images must have matching dimensions");
    double result = 0;
    for (unsigned y = firstRow; y < a.height; ++y)
        for (unsigned x = 0; x < a.width; ++x)
            for (unsigned channel = 0; channel < 3; ++channel)
                result += std::abs(a.pixels[size_t(y) * a.width + x][channel] - b.pixels[size_t(y) * a.width + x][channel]);
    return result / (double(a.width) * (a.height - firstRow) * 3);
}

// 排除轮廓边缘，只比较 Cornell 高盒子的光泽正面区域。
double GlossyDifference(const Image &a, const Image &b) {
    Require(a.width == b.width && a.height == b.height, "Glossy comparisons require equal image dimensions");
    const unsigned left = a.width * 53 / 100, right = a.width * 63 / 100;
    const unsigned top = a.height * 58 / 100, bottom = a.height * 86 / 100;
    double error = 0;
    for (unsigned y = top; y < bottom; ++y)
        for (unsigned x = left; x < right; ++x)
            for (unsigned channel = 0; channel < 3; ++channel)
                error += std::abs(a.pixels[size_t(y) * a.width + x][channel] -
                                  b.pixels[size_t(y) * b.width + x][channel]);
    return error / (double(right - left) * (bottom - top) * 3);
}

// 下半表面的百分位指标可发现整面平均值掩盖的局部亮点。
double GlossyBottomOutlier(const Image &a, const Image &b) {
    std::vector<double> errors;
    for (unsigned y = a.height * 79 / 100; y < a.height * 86 / 100; ++y)
        for (unsigned x = a.width * 53 / 100; x < a.width * 63 / 100; ++x) {
            double error = 0;
            for (unsigned channel = 0; channel < 3; ++channel)
                error = std::max(error, std::abs(a.pixels[size_t(y) * a.width + x][channel] -
                                               b.pixels[size_t(y) * b.width + x][channel]));
            errors.push_back(error);
        }
    const auto percentile = errors.begin() + errors.size() * 95 / 100;
    std::nth_element(errors.begin(), percentile, errors.end());
    return *percentile;
}

class ImageReadback {
  public:
    ImageReadback(render::dx12::Dx12Device &device, render::dx12::ResourcePool &resources)
        : device_(device), resources_(resources) {}
    Image Read(render::dx12::GpuResource &image) {
        const auto description = image.object->GetDesc();
        const bool hdr = description.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        Require(hdr || description.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
                "Integration readback requires float HDR or RGBA8 display output");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 totalBytes{};
        device_.Native()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
        if (!readback_ || capacity_ < totalBytes) {
            readback_ = &resources_.Buffer("Integration.DisplayReadback", totalBytes, 0, D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
            capacity_ = totalBytes;
        }

        // 与帧共用的图执行器负责读回转换，并恢复准确显示状态。
        const auto originalState = image.state;
        device_.BeginCommands();
        render::RenderGraph graph;
        render::dx12::GraphExecutor executor(device_);
        executor.Import(graph, resources_);
        graph.AddPass(
            "Integration.DisplayReadback",
            {render::dx12::Read(image, D3D12_RESOURCE_STATE_COPY_SOURCE),
             render::dx12::Write(*readback_, D3D12_RESOURCE_STATE_COPY_DEST)},
            [&] {
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = image.object.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION target{};
                target.pResource = readback_->object.Get();
                target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                target.PlacedFootprint = footprint;
                device_.Commands()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            },
            true);
        graph.Export(readback_->graphId, D3D12_RESOURCE_STATE_COPY_DEST);
        graph.Export(image.graphId, originalState);
        executor.Execute(graph);
        device_.SubmitAndWait();
        Require(image.state == originalState, "Numerical readback must restore the image state");

        // 传输断言保留原始 HDR，仅为已有图像比较解码显示 sRGB。
        Image result;
        result.width = static_cast<unsigned>(description.Width);
        result.height = description.Height;
        result.pixels.resize(size_t(result.width) * result.height);
        void *mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
        render::dx12::Check(readback_->object->Map(0, &range, &mapped), "Map integration image");
        const auto *bytes = static_cast<const std::uint8_t *>(mapped);
        for (unsigned y = 0; y < result.height; ++y)
            for (unsigned x = 0; x < result.width; ++x)
                for (unsigned channel = 0; channel < 3; ++channel) {
                    double value;
                    if (hdr) {
                        float linear;
                        std::memcpy(&linear, bytes + size_t(y) * footprint.Footprint.RowPitch + (x * 4 + channel) * sizeof(float), sizeof(float));
                        Require(std::isfinite(linear), "Float frame images must contain finite values");
                        value = linear;
                    } else {
                        const double encoded = bytes[size_t(y) * footprint.Footprint.RowPitch + x * 4 + channel] / 255.0;
                        value = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
                    }
                    result.pixels[size_t(y) * result.width + x][channel] = value;
                }
        const D3D12_RANGE written{0, 0};
        readback_->object->Unmap(0, &written);
        return result;
    }

  private:
    render::dx12::Dx12Device &device_;      // 同步 GPU 提交所有者。
    render::dx12::ResourcePool &resources_; // 可复用读回缓冲区的所有者。
    render::dx12::GpuResource *readback_{}; // 比较之间复用的地址稳定资源池分配。
    UINT64 capacity_{};                     // 当前可用的最大读回布局尺寸。
};

struct HiddenWindow {
    HWND handle{}; // 真实隐藏显示目标，晚于全部 GPU 对象销毁。

    HiddenWindow() {
        WNDCLASSEXW description{sizeof(WNDCLASSEXW)};
        description.lpfnWndProc = DefWindowProcW;
        description.hInstance = GetModuleHandleW(nullptr);
        // 独立窗口类隔离渲染器集成测试，避免与交互编辑器复用原生消息状态。
        description.lpszClassName = L"EncoreIntegrationWindow";
        Require(RegisterClassExW(&description) != 0, "Register test window class");
        handle = CreateWindowExW(0, description.lpszClassName, L"Renderer integration", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                                 nullptr, nullptr, description.hInstance, nullptr);
        Require(handle != nullptr, "Create hidden test window");
    }
    ~HiddenWindow() {
        if (handle)
            DestroyWindow(handle);
    }
};

bool HasPass(const render::FrameRenderer &renderer, const char *name) {
    const auto &passes = renderer.PassNames();
    return std::find(passes.begin(), passes.end(), name) != passes.end();
}

// 检查显式外部诊断快照；瞬时分配在图中末次使用后失效。
render::dx12::GpuResource &FrameImage(render::dx12::ResourcePool &resources, const char *name) {
    for (const auto &resource : resources.All())
        if (resource->name == name) return *resource;
    throw std::runtime_error(std::string("Missing frame image: ") + name);
}

// 每个场景使用真实显示及与编辑器一致的显式有序最终覆盖层。
void Draw(render::FrameRenderer &renderer, gameplay::World &world, render::RenderSettings &settings,
          render::dx12::Dx12Device &device, render::dx12::DescriptorArena &descriptors, render::dx12::Dx12Swapchain &swapchain) {
    // 图像断言按需启用拷贝提取，不改变生产图的资源保留行为。
    renderer.RetainFrameImages();
    auto &backbuffer = swapchain.Current();
    const auto snapshot = world.ExtractRenderSnapshot();
    renderer.Draw(snapshot, settings, 1.0f / 60, backbuffer, [&](render::RenderGraph &graph) {
        const auto overlay = graph.AddPass(
            "Integration.FinalOverlay",
            {render::dx12::Read(renderer.Display()), render::dx12::Write(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET)}, [&] {
                const float color[]{0.08f, 0.085f, 0.09f, 1};
                device.Commands()->ClearRenderTargetView(descriptors.Rtv(swapchain.CurrentIndex()), color, 0, nullptr);
            });
        // 最终覆盖层只约束存活工作的顺序，避免测试改变正式帧的裁剪结果。
        for (render::PassId before = 0; before < overlay; ++before)
            graph.AddOrdering(before, overlay);
    });
    Require(!renderer.PassNames().empty() && renderer.PassNames().back() == "Integration.FinalOverlay",
            "Overlay must execute last");
    swapchain.Present(false);
}

void CheckShadowGraph(const render::FrameRenderer &renderer, unsigned mode, bool enabled = true) {
    const bool raster = enabled && mode != 0;
    const bool variance = enabled && mode == 3;
    Require(HasPass(renderer, "Shadow.RasterDepth") == raster, "Selected shadow mode must schedule its raster production pass");
    Require(HasPass(renderer, "Shadow.BlurHorizontal") == variance, "Only VSM must schedule horizontal moment filtering");
    Require(HasPass(renderer, "Shadow.BlurVertical") == variance, "Only VSM must schedule vertical moment filtering");
}

// 仅直接光比较排除随机间接路径及全部时间、空间重建影响。
void DirectOnly(render::RenderSettings &settings) {
    settings.gi = 1;
    settings.giIntensity = 0;
    settings.samples = 1;
    settings.pathVertices = 1;
    settings.temporalReuse = settings.spatialReuse = false;
    settings.bloom = 0;
    settings.mediumDensity = 0;
    settings.autoExposure = false;
    settings.exposure = -1;
}

// 独立导入样例同时提供真实 UV、命名部件和已解码图像字节。
render::Scene ImportedFixture() {
    auto scene = render::ImportGltf(std::filesystem::path(RENDER_SOURCE_DIRECTORY) / L"tests/fixtures/textured_multilight.gltf");
    Require(scene.meshParts.size() == 2 && scene.lights.size() == 3 && !scene.attributes.empty() && !scene.packedTexels.empty(),
            "Imported integration fixture must retain its geometry, lights, UV attributes and image pixels");
    for (unsigned image = 0; image < 2; ++image) {
        render::GpuTextureView view;
        view.image = {static_cast<unsigned>(scene.packedTexels.size()), 2, 2, 0};
        view.offsetScale = {0, 0, 1, 1};
        view.rotation = {0, 1, 0, 0};
        scene.textureViews.push_back(view);
        const std::uint32_t flat = 0xffff8080u;
        const std::uint32_t left = 0xffd080e8u;
        const std::uint32_t right = 0xffd08018u;
        for (unsigned texel = 0; texel < 4; ++texel)
            scene.packedTexels.push_back(image == 0 ? flat : texel % 2 == 0 ? left : right);
    }
    return scene;
}

// 房间网格光源独立于解析光所有权，与导入自发光材质保持一致。
render::Scene EmissiveMeshFixture() {
    auto scene = render::BuildCornellScene();
    scene.cornell = false;
    for (auto &material : scene.materials) {
        material.baseRoughness = {0.73f, 0.73f, 0.73f, 0.78f};
        material.emissionMetallic = {};
        material.principled.x = 0;
    }
    scene.materials[3].baseRoughness = {0, 0, 0, 0.78f};
    return scene;
}
void FrameResourceContracts(render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors) {
    using namespace render;
    using namespace render::dx12;
    const ResourceDescriptor texture{ResourceKind::Texture2D, 32, 24, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
        static_cast<unsigned>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), {}};
    const ResourceDescriptor buffer{ResourceKind::Buffer, 1024, 1, 0, 4,
        static_cast<unsigned>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), {}};
    const auto rejects = [](const auto& create, const char* message) {
        bool rejected = false;
        try { create(); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, message);
    };
    // 类型边界必须在逻辑描述阶段生效，不能等到实际分配后再区分纹理与缓冲区。
    rejects([&] { GraphTexture invalid("Invalid.Texture", buffer); }, "GraphTexture rejects buffer descriptions");
    rejects([&] { GraphBuffer invalid("Invalid.Buffer", texture); }, "GraphBuffer rejects texture descriptions");
    rejects([&] { RenderTarget invalid("Invalid.Target", texture); }, "RenderTarget rejects UAV-only textures");
    auto color = texture;
    color.flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    RenderTarget colorTarget("Valid.Color", color);
    auto depth = texture;
    depth.format = DXGI_FORMAT_D32_FLOAT;
    depth.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    RenderTarget depthTarget("Valid.Depth", depth);
    Require(!colorTarget.Resource().object && !depthTarget.Resource().object,
        "Valid color and depth target descriptions do not allocate native storage");

    // 分组声明只能登记逻辑资源，历史与显示之外的物理分配必须等待图执行。
    ResourcePool pool(device, descriptors);
    FrameResources frame(descriptors, pool, 32, 24);
    const auto externalCount = pool.All().size();
    RenderGraph graph;
    GraphExecutor executor(device);
    executor.Import(graph, pool);
    frame.DeclareFrame(graph, executor);
    Require(pool.All().size() == externalCount && pool.TransientAllocationCount() == 0,
        "Declaring frame resource groups does not allocate native transient storage");
    const auto transientCount = std::count_if(graph.Resources().begin(), graph.Resources().end(),
        [](const GraphResource& resource) { return resource.transient; });
    Require(transientCount == 19, "All frame groups register their logical resources");
    Require(!frame.gbuffer.position.Resource().object && !frame.lighting.gi.Resource().object &&
        !frame.restir.candidates.Resource().object && !frame.denoiser.filtered.Resource().object && !frame.effects.subsurface.Resource().object,
        "Raster, lighting, reservoir, denoiser and effect proxies remain unmaterialized during setup");

    // 零拷贝历史交换要求能力相同，尤其不能丢失位置和法线源纹理的 RTV 能力。
    Require(static_cast<unsigned>(frame.history.position->object->GetDesc().Flags) == frame.gbuffer.position.Description().flags &&
        static_cast<unsigned>(frame.history.normal->object->GetDesc().Flags) == frame.gbuffer.normal.Description().flags &&
        static_cast<unsigned>(frame.history.gi->object->GetDesc().Flags) == frame.lighting.gi.Description().flags &&
        static_cast<unsigned>(frame.history.resolved->object->GetDesc().Flags) == frame.denoiser.resolved.Description().flags &&
        static_cast<unsigned>(frame.history.reservoir->object->GetDesc().Flags) == frame.restir.spatial.Description().flags,
        "History allocations exactly match extraction source capabilities");
    std::cout << "PASS frame resource types, target capabilities, grouped setup and history compatibility\n";
}

void GraphTransientStorage(render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors) {
    using namespace render;
    using namespace render::dx12;
    ResourcePool pool(device, descriptors);
    auto& readback = pool.Buffer("RDG.ExtractedReadback", 256, 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    const ResourceDescriptor description{ResourceKind::Buffer, 1024, 1, 0, 4,
        static_cast<unsigned>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), {}};
    GpuResource first, second, dead;
    ID3D12Resource* reused = nullptr;
    device.BeginCommands();
    descriptors.Bind(device.Commands());
    RenderGraph graph;
    GraphExecutor executor(device);
    executor.Import(graph, pool);
    executor.Create(graph, first, "RDG.First", description);
    executor.Create(graph, second, "RDG.Second", description);
    executor.Create(graph, dead, "RDG.Culled", description);
    const auto clear = [&](GpuResource& target, UINT value) {
        const UINT values[]{value, value, value, value};
        device.Commands()->ClearUnorderedAccessViewUint(descriptors.Gpu(target.clearUav), descriptors.Cpu(target.clearUav),
            target.object.Get(), values, 0, nullptr);
    };
    // 不重叠寿命的两个值应共享存储，同时保留各自提取的字节。
    graph.AddPass("RDG.FirstWrite", {Write(first)}, [&] { reused = first.object.Get(); clear(first, 17); });
    graph.AddPass("RDG.FirstExtract", {Read(first, D3D12_RESOURCE_STATE_COPY_SOURCE), Write(readback, D3D12_RESOURCE_STATE_COPY_DEST)},
        [&] { device.Commands()->CopyBufferRegion(readback.object.Get(), 0, first.object.Get(), 0, 4); });
    graph.AddPass("RDG.SecondWrite", {Write(second)}, [&] {
        Require(second.object.Get() == reused, "Compatible nonoverlapping RDG lifetimes must reuse the native allocation");
        clear(second, 29);
    });
    graph.AddPass("RDG.SecondExtract", {Read(second, D3D12_RESOURCE_STATE_COPY_SOURCE), Write(readback, D3D12_RESOURCE_STATE_COPY_DEST)},
        [&] { device.Commands()->CopyBufferRegion(readback.object.Get(), 4, second.object.Get(), 0, 4); });
    graph.AddPass("RDG.CulledWrite", {Write(dead)}, [] { throw std::runtime_error("Culled transient callback executed"); });
    graph.Export(readback.graphId, D3D12_RESOURCE_STATE_COPY_DEST);
    executor.Execute(graph);
    Require(pool.TransientAllocationCount() == 1, "Culled resources allocate nothing and disjoint lifetimes use one physical allocation");
    Require(!first.object && !second.object && !dead.object, "Transient native handles expire at their last retained use");
    device.SubmitAndWait();
    void* mapped = nullptr;
    const D3D12_RANGE readable{0, 8};
    Check(readback.object->Map(0, &readable, &mapped), "Map extracted RDG values");
    const auto* values = static_cast<const UINT*>(mapped);
    const bool valid = values[0] == 17 && values[1] == 29;
    const D3D12_RANGE written{0, 0};
    readback.object->Unmap(0, &written);
    Require(valid, "Actual GPU extraction preserves both values across physical storage reuse");

    // fence 完成后另一张图可继续复用，而不增长物理缓存。
    device.BeginCommands();
    descriptors.Bind(device.Commands());
    RenderGraph next;
    GraphExecutor nextExecutor(device);
    nextExecutor.Import(next, pool);
    nextExecutor.Create(next, first, "RDG.NextFrame", description);
    next.AddPass("RDG.NextFrameWrite", {Write(first)}, [&] {
        Require(first.object.Get() == reused, "Completed prior-frame allocations must be reused");
        clear(first, 41);
    }, true);
    nextExecutor.Execute(next);
    device.SubmitAndWait();
    Require(pool.TransientAllocationCount() == 1, "Repeated graph execution must not grow compatible transient storage");
    std::cout << "PASS GPU RDG transient allocation, culling, native reuse, extraction and fence retirement\n";
}

void GraphParameterUploads(render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors) {
    using namespace render;
    using namespace render::dx12;
    ResourcePool pool(device, descriptors);
    auto& readback = pool.Buffer("Parameters.Readback", 256, 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    auto& sceneData = pool.Buffer("Parameters.SceneData", 256);
    RenderGraph graph;
    GraphExecutor executor(device);
    executor.Import(graph, pool);
    std::array<std::uint32_t, 4> first{17, 19, 23, 29}, second{31, 37, 41, 43};
    ID3D12Resource* firstNative{};
    auto& firstUpload = executor.UploadParameters(graph, "Parameters.First", std::as_bytes(std::span(first)));
    PassParameters firstParameters;
    firstParameters.Access(firstUpload, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
    firstParameters.Access(readback, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    firstParameters.AddPass(graph, "Parameters.First.Copy", [&](const auto&) {
        firstNative = firstUpload.object.Get();
        device.Commands()->CopyBufferRegion(readback.object.Get(), 0, firstUpload.object.Get(), 0, sizeof(first));
    });
    auto& secondUpload = executor.UploadParameters(graph, "Parameters.Second", std::as_bytes(std::span(second)));
    PassParameters secondParameters;
    secondParameters.Access(secondUpload, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
    secondParameters.Access(readback, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    secondParameters.AddPass(graph, "Parameters.Second.Copy", [&](const auto&) {
        Require(secondUpload.object.Get() != firstNative,
            "Disjoint GPU uses must not alias CPU upload bytes within one submission");
        device.Commands()->CopyBufferRegion(readback.object.Get(), sizeof(first), secondUpload.object.Get(), 0, sizeof(second));
    });
    auto& unused = executor.UploadParameters(graph, "Parameters.Unused", std::as_bytes(std::span(first)));
    // 场景上传的兼容接口也必须保存同图内两次修改，不能让后一次 CPU 写入覆盖第一次。
    const auto sceneCopy = [&](UINT64 offset) {
        PassParameters parameters;
        parameters.Access(sceneData, ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
        parameters.Access(readback, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
        parameters.AddPass(graph, "Parameters.Scene.Copy", [&, offset](const auto&) {
            device.Commands()->CopyBufferRegion(readback.object.Get(), offset, sceneData.object.Get(), 0, 4);
        });
    };
    pool.Upload(graph, executor, sceneData, first.data(), sizeof(first));
    sceneCopy(32);
    pool.Upload(graph, executor, sceneData, second.data(), sizeof(second));
    sceneCopy(36);
    // 改写建图端栈数据，实际提交仍必须读取各 Pass 自己拥有的参数快照。
    first.fill(0);
    second.fill(0);
    Require(!firstUpload.object && !secondUpload.object && !unused.object && pool.TransientAllocationCount() == 0,
        "Parameter setup must not allocate or map native upload storage");
    bool denied = false;
    try { firstParameters.Validate(graph); } catch (const std::logic_error&) { denied = true; }
    Require(denied, "Parameter native access must be denied outside its executing pass");
    device.BeginCommands();
    executor.Execute(graph);
    Require(graph.Lifetimes()[unused.graphId].allocation == InvalidGraphIndex && pool.TransientAllocationCount() == 4,
        "Unused parameter uploads must be culled with no physical allocation");
    device.SubmitAndWait();
    void* mapped{};
    const D3D12_RANGE readable{0, 40}, written{0, 0};
    Check(readback.object->Map(0, &readable, &mapped), "Read parameter snapshot results");
    const std::array<std::uint32_t, 10> expected{17, 19, 23, 29, 31, 37, 41, 43, 17, 31};
    const bool correct = std::memcmp(mapped, expected.data(), sizeof(expected)) == 0;
    readback.object->Unmap(0, &written);
    Require(correct, "GPU commands must observe independently captured parameter values");
    std::cout << "PASS per-pass parameter ownership, deferred upload, access guard, CPU alias protection and culling\n";
}

void GraphPendingUploadsWaitIdle(render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors) {
    using namespace render;
    using namespace render::dx12;
    // 两张图共用一次提交，空闲信号不能替尚未执行的上传命令结束资源寿命。
    ResourcePool pool(device, descriptors);
    auto& readback = pool.Buffer("PendingParameters.Readback", 256, 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    RenderGraph first;
    GraphExecutor firstExecutor(device);
    firstExecutor.Import(first, pool);
    const std::array<std::uint32_t, 1> firstValue{17};
    auto& firstUpload = firstExecutor.UploadParameters(first, "PendingParameters.First", std::as_bytes(std::span(firstValue)));
    ID3D12Resource* firstNative{};
    PassParameters firstCopy;
    firstCopy.Access(firstUpload, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
    firstCopy.Access(readback, ResourceAccess::ReadWrite, D3D12_RESOURCE_STATE_COPY_DEST);
    firstCopy.AddPass(first, "PendingParameters.First.Copy", [&](const auto&) {
        firstNative = firstUpload.object.Get();
        device.Commands()->CopyBufferRegion(readback.object.Get(), 0, firstUpload.object.Get(), 0, sizeof(firstValue));
    });
    device.BeginCommands();
    firstExecutor.Execute(first);
    device.WaitIdle();

    // 不重置命令列表，第二张图的 CPU 写入必须与第一张图待提交的数据隔离。
    RenderGraph second;
    GraphExecutor secondExecutor(device);
    secondExecutor.Import(second, pool);
    const std::array<std::uint32_t, 1> secondValue{29};
    auto& secondUpload = secondExecutor.UploadParameters(second, "PendingParameters.Second", std::as_bytes(std::span(secondValue)));
    ID3D12Resource* secondNative{};
    PassParameters secondCopy;
    secondCopy.Access(secondUpload, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
    secondCopy.Access(readback, ResourceAccess::ReadWrite, D3D12_RESOURCE_STATE_COPY_DEST);
    secondCopy.AddPass(second, "PendingParameters.Second.Copy", [&](const auto&) {
        secondNative = secondUpload.object.Get();
        device.Commands()->CopyBufferRegion(readback.object.Get(), sizeof(firstValue), secondUpload.object.Get(), 0, sizeof(secondValue));
    });
    secondExecutor.Execute(second);
    device.SubmitAndWait();

    // 读取真实 GPU 结果，防止仅用句柄不同掩盖提前覆盖上传字节的问题。
    void* mapped{};
    const D3D12_RANGE readable{0, sizeof(firstValue) + sizeof(secondValue)}, written{0, 0};
    Check(readback.object->Map(0, &readable, &mapped), "Read pending parameter upload results");
    std::array<std::uint32_t, 2> values{};
    std::memcpy(values.data(), mapped, sizeof(values));
    readback.object->Unmap(0, &written);
    std::cout << "Pending upload readback: " << values[0] << ',' << values[1]
        << "; distinct allocations=" << (firstNative != secondNative) << '\n';
    Require(values == std::array<std::uint32_t, 2>{17, 29},
        "WaitIdle must not retire unsubmitted upload bytes: expected 17,29");
    Require(firstNative != secondNative, "Separate pending graphs must not share a CPU upload allocation");
    std::cout << "PASS pending upload lifetime across WaitIdle and multiple graphs in one submission\n";
}

void GraphHistoryExtraction(render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors) {
    using namespace render;
    using namespace render::dx12;
    ResourcePool pool(device, descriptors);
    const ResourceDescriptor description{ResourceKind::Buffer, 1024, 1, 0, 4,
        static_cast<unsigned>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), {}};
    auto& history = pool.Buffer("History.Value", 1024, 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& otherHistory = pool.Buffer("History.Other", 1024, 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& incompatible = pool.Buffer("History.WrongSize", 512, 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& rawHistory = pool.Buffer("History.Raw", 1024);
    auto& smallStructured = pool.Buffer("History.SmallStructured", 256, 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& readback = pool.Buffer("History.Readback", 1024, 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    auto* original = history.object.Get();
    GpuResource source, otherSource;
    GraphExecutor executor(device);
    const auto clear = [&](UINT value) {
        const UINT values[]{value, value, value, value};
        device.Commands()->ClearUnorderedAccessViewUint(descriptors.Gpu(source.clearUav), descriptors.Cpu(source.clearUav),
            source.object.Get(), values, 0, nullptr);
    };
    RenderGraph first;
    executor.Import(first, pool);
    executor.Create(first, source, "History.First", description);
    executor.Create(first, otherSource, "History.OtherFirst", description);
    ID3D12Resource* produced{};
    PassParameters write;
    write.Uav(0, source);
    write.AddPass(first, "History.First.Write", [&](const auto&) { produced = source.object.Get(); clear(73); });
    bool rejected = false;
    try { executor.QueueExtraction(first, "History.Invalid", source, incompatible); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected && history.object.Get() == original,
        "An incompatible extraction must fail during setup without publishing valid earlier requests");
    executor.QueueExtraction(first, "History.Publish", source, history);
    // 相同尺寸仍不足以交换上传、默认及读回堆，防止破坏缓存的可映射性。
    auto plain = description;
    plain.flags = plain.stride = 0;
    rejected = false;
    try { pool.ValidateExtraction(plain, readback); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Default-heap history extraction rejects a matching readback heap");
    plain.memory = ResourceMemory::Upload;
    rejected = false;
    try { pool.ValidateExtraction(plain, rawHistory); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Upload resources cannot be published into the default-heap history cache");
    auto compactDescription = description;
    compactDescription.width = 128;
    rejected = false;
    try { pool.ValidateExtraction(compactDescription, smallStructured); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Matching native minimum sizes must not hide incompatible structured view counts");
    PassParameters otherWrite;
    otherWrite.Uav(0, otherSource);
    otherWrite.AddPass(first, "History.Other.Write", [&](const auto&) {
        const UINT values[]{111, 111, 111, 111};
        device.Commands()->ClearUnorderedAccessViewUint(descriptors.Gpu(otherSource.clearUav), descriptors.Cpu(otherSource.clearUav),
            otherSource.object.Get(), values, 0, nullptr);
    });
    executor.QueueExtraction(first, "History.Other.Publish", otherSource, otherHistory);
    // 建图后外部目标被替换也必须在任何发布之前整体拒绝，不能让第一项先交换成功。
    auto savedOther = otherHistory.object;
    otherHistory.object = incompatible.object;
    device.BeginCommands();
    descriptors.Bind(device.Commands());
    rejected = false;
    try { executor.Execute(first); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected && history.object.Get() == original && pool.TransientAllocationCount() == 0,
        "All extraction targets must be revalidated before recording or publishing any result");
    otherHistory.object = std::move(savedOther);
    executor.Execute(first);
    Require(history.object.Get() == produced && produced != original && !source.object,
        "Completed graph extraction transfers native ownership with no copy and expires the transient proxy");
    // 空闲 fence 不包含当前已录制命令，不能提前回收轮换进缓存的旧历史。
    device.WaitIdle();
    auto& inFlight = pool.AcquireTransient(description, "History.FenceProbe");
    Require(inFlight.object.Get() != original && inFlight.object.Get() != produced,
        "Old history stays fenced until the commands that read it have completed");
    device.SubmitAndWait();

    // 同一执行器进入新图必须清除旧提取队列，并可再次发布同一历史目标。
    RenderGraph next;
    executor.Import(next, pool);
    executor.Create(next, source, "History.Second", description);
    PassParameters previous;
    previous.Access(history, ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    previous.Access(readback, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    previous.AddPass(next, "History.Previous.Copy", [&](const auto&) {
        device.Commands()->CopyBufferRegion(readback.object.Get(), 0, history.object.Get(), 0, 4);
    });
    PassParameters current;
    current.Uav(0, source);
    current.AddPass(next, "History.Second.Write", [&](const auto&) { clear(97); });
    executor.QueueExtraction(next, "History.Publish", source, history);
    device.BeginCommands();
    descriptors.Bind(device.Commands());
    executor.Execute(next);
    device.SubmitAndWait();
    RenderGraph read;
    executor.Import(read, pool);
    PassParameters latest;
    latest.Access(history, ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    latest.Access(readback, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    latest.AddPass(read, "History.Latest.Copy", [&](const auto&) {
        device.Commands()->CopyBufferRegion(readback.object.Get(), 4, history.object.Get(), 0, 4);
    });
    device.BeginCommands();
    executor.Execute(read);
    device.SubmitAndWait();
    void* mapped{};
    const D3D12_RANGE readable{0, 8}, written{0, 0};
    Check(readback.object->Map(0, &readable, &mapped), "Read history extraction results");
    const auto* values = static_cast<const UINT*>(mapped);
    const bool correct = values[0] == 73 && values[1] == 97;
    readback.object->Unmap(0, &written);
    Require(correct, "Consecutive zero-copy publications preserve previous and current GPU results");
    std::cout << "PASS zero-copy extraction, heap validation, setup rejection, fenced ownership and executor reuse\n";
}
// 使用完整同步计数区分 CPU 重建与已有的 GPU 上传跳过，避免性能回归被静态画面掩盖。
void CameraUpdateIsolation(render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors) {
    render::dx12::ResourcePool pool(device, descriptors);
    render::SceneGpu scene(device, descriptors, pool);
    gameplay::World world;
    world.CreateCornell(scene.ReferenceScene());
    Require(scene.Synchronize(world.ExtractRenderSnapshot()), "Initial snapshot must synchronize the scene");
    const auto builds = scene.SceneBuildCount();
    Require(builds == 1, "Initial snapshot must build scene geometry once");
    Require(!scene.Synchronize(world.ExtractRenderSnapshot()), "An accepted revision needs no second synchronization");

    // 位姿、投影和活动相机集合只改变视图，不能触发场景提取或软件 BVH 构建。
    auto& pose = world.Registry().get<gameplay::Transform>(world.CameraEntity());
    pose.position.x += .25f;
    pose.rotationDegrees = {10, 20, 30};
    world.MarkDirty();
    Require(scene.Synchronize(world.ExtractRenderSnapshot()), "Camera revisions still report history invalidation");
    Require(scene.SceneBuildCount() == builds, "Camera-only revisions must skip scene extraction and CPU BVH construction");
    auto& camera = world.Registry().get<gameplay::Camera>(world.CameraEntity());
    camera.verticalFovDegrees = 65;
    camera.nearPlane = .1f;
    camera.farPlane = 250;
    world.MarkDirty();
    Require(scene.Synchronize(world.ExtractRenderSnapshot()) && scene.SceneBuildCount() == builds,
        "Projection changes preserve the accepted scene geometry");
    auto changed = world.ExtractRenderSnapshot();
    ++changed.revision;
    changed.cameras.clear();
    Require(scene.Synchronize(changed) && scene.SceneBuildCount() == builds,
        "Switching to the fallback camera preserves scene geometry");

    // 快路径也验证未被选中的相机；失败不能提前接受版本，修正后可用相同版本重试。
    ++changed.revision;
    changed.cameras = world.ExtractRenderSnapshot().cameras;
    changed.cameras.push_back(changed.cameras.front());
    changed.cameras.back().camera.nearPlane = -1;
    bool rejected = false;
    try { scene.Synchronize(changed); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected && scene.SceneBuildCount() == builds, "Invalid secondary camera rejects without rebuilding or accepting its revision");
    changed.cameras.back().camera.nearPlane = .1f;
    Require(scene.Synchronize(changed) && scene.SceneBuildCount() == builds,
        "A rejected camera revision can be corrected and retried");

    // 材质、生成发光体与网格输入继续走完整路径，不被相机分类吞掉。
    ++changed.revision;
    changed.materials.front().baseRoughness.x += .1f;
    Require(scene.Synchronize(changed) && scene.SceneBuildCount() == builds + 1, "Material edits must synchronize scene inputs");
    ++changed.revision;
    changed.lights.front().light.intensity += 1;
    Require(scene.Synchronize(changed) && scene.SceneBuildCount() == builds + 2, "Emitter radiance edits must synchronize generated materials");
    ++changed.revision;
    changed.instances.front().transform.position.x += .1f;
    Require(scene.Synchronize(changed) && scene.SceneBuildCount() == builds + 3, "Instance edits must rebuild transformed geometry");
    std::cout << "PASS camera-only scene isolation, camera validation retry and non-camera invalidation\n";
}
} // 命名空间结束

int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        // 调查光线边界故障时，接触回归可独立运行。
        const bool contactOnly = argc == 2 && std::string(argv[1]) == "--contact-only";
        std::cout << "Begin GPU integration\n";
        // 使用真实同步队列和 RDG 执行器验证，不依赖编辑器或模拟 GPU 资源。
        HiddenWindow window;
        render::dx12::Dx12Device device(true);
        render::dx12::ComPtr<ID3D12InfoQueue1> diagnostics;
        if (SUCCEEDED(device.Native()->QueryInterface(IID_PPV_ARGS(&diagnostics)))) {
            DWORD cookie{};
            diagnostics->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR message, void *) {
                    if (severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                        std::cerr << "GPU callback: " << message << '\n';
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
        render::dx12::DescriptorArena descriptors(device);
        FrameResourceContracts(device, descriptors);
        GraphTransientStorage(device, descriptors);
        GraphParameterUploads(device, descriptors);
        GraphPendingUploadsWaitIdle(device, descriptors);
        GraphHistoryExtraction(device, descriptors);
        CameraUpdateIsolation(device, descriptors);
        if (argc == 2 && std::string(argv[1]) == "--rdg-only") return 0;
        render::dx12::ResourcePool resources(device, descriptors);
        render::dx12::Dx12Swapchain swapchain(device, descriptors, window.handle);
        if (!contactOnly) {
        {
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120);
            std::cout << "Created frame renderer\n";
            render::CaptureService capture(device, resources);
            ImageReadback readback(device, resources);
            gameplay::World world;
            world.CreateCornell(renderer.ReferenceScene());
            render::RenderSettings settings;
            const auto draw = [&] { Draw(renderer, world, settings, device, descriptors, swapchain); };
            const unsigned backendCount = device.DxrSupported() ? 2u : 1u;

            // 较大的显示采样数应对应稳定光泽反射，不应保留永久 reservoir 斑块。
            settings.gi = 0;
            settings.samples = 16;
            settings.pathVertices = 4;
            settings.bloom = 0;
            for (unsigned frame = 0; frame < 1024; ++frame) draw();
            const auto pathReference = readback.Read(renderer.Display());
            capture.Capture(renderer.Display(), L"captures/integration-contact-metal-pt.png");
            // 独立 PT 与 ReSTIR 使用相同有限采样预算，以实测噪声约束比较。
            settings.reset = true;
            for (unsigned frame = 0; frame < 256; ++frame) draw();
            const auto pathControl = readback.Read(renderer.Display());
            const double referenceNoise = GlossyDifference(pathReference, pathControl);
            const double referenceOutlier = GlossyBottomOutlier(pathReference, pathControl);
            std::cout << "Glossy PT finite-budget noise=" << referenceNoise << " bottom P95=" << referenceOutlier << '\n';
            for (unsigned backend = 0; backend < backendCount; ++backend) {
                settings.backend = backend;
                settings.gi = 2;
                settings.candidates = 2;
                settings.reset = true;
                for (unsigned frame = 0; frame < 8; ++frame) draw();
                const auto earlyReSTIR = readback.Read(renderer.Display());
                for (unsigned frame = 8; frame < 224; ++frame) draw();
                const auto lateReSTIR = readback.Read(renderer.Display());
                for (unsigned frame = 224; frame < 256; ++frame) draw();
                const auto finalReSTIR = readback.Read(renderer.Display());
                const double earlyError = GlossyDifference(pathReference, earlyReSTIR);
                const double lateError = GlossyDifference(pathReference, finalReSTIR);
                const double lateDrift = GlossyDifference(lateReSTIR, finalReSTIR);
                const double bottomOutlier = GlossyBottomOutlier(pathReference, finalReSTIR);
                std::cout << "Glossy ReSTIR backend=" << backend << " early error=" << earlyError << " late error=" << lateError
                          << " late drift=" << lateDrift << " bottom P95=" << bottomOutlier << '\n';
                capture.Capture(renderer.Display(), backend == 0 ? L"captures/integration-restir-glossy.png"
                                                                : L"captures/integration-restir-glossy-dxr.png");
                Require(lateDrift < 0.01, "Accumulated static ReSTIR glossy lighting must settle instead of retaining moving blotches");
                Require(lateError < earlyError * 0.7 && lateError < referenceNoise * 1.5 + 0.001,
                        "Accumulated glossy ReSTIR lighting must approach the same-scene path-traced reference");
                Require(bottomOutlier < referenceOutlier * 1.5 + 0.005,
                        "The lower glossy face must not retain bright outliers beyond independent PT sampling noise");
            }
            // 漫反射对照用于区分地板、墙面的反射与穿过不透明盒子的漏光。
            const auto smallBoxMaterial = world.Materials()[4];
            const auto tallBoxMaterial = world.Materials()[5];
            for (unsigned index : {4u, 5u}) {
                auto &material = world.Materials()[index];
                material.baseRoughness = {0.73f, 0.73f, 0.73f, 0.78f};
                material.emissionMetallic.w = 0;
                material.transmissionIor = {0, 1.5f, 0, 0};
                material.principled.x = 0;
            }
            world.MarkDirty();
            settings.backend = 0;
            settings.gi = 0;
            for (unsigned frame = 0; frame < 256; ++frame) draw();
            const auto matteControl = readback.Read(renderer.Display());
            capture.Capture(renderer.Display(), L"captures/integration-contact-matte-pt.png");
            Require(GlossyDifference(pathReference, matteControl) > referenceNoise * 3,
                    "Replacing the metal with opaque diffuse must remove its reflected floor/wall appearance");
            world.Materials()[4] = smallBoxMaterial;
            world.Materials()[5] = tallBoxMaterial;
            world.MarkDirty();
            settings = render::RenderSettings{};

            // 模式切换共用资源池和描述符，以暴露旧历史或图编号错误。
            for (unsigned backend = 0; backend < backendCount; ++backend) {
                for (unsigned gi = 0; gi < 3; ++gi) {
                    settings.backend = backend;
                    settings.gi = gi;
                    settings.reset = true;
                    std::cout << "Test backend=" << backend << " GI=" << gi << '\n';
                    draw();
                    Require(renderer.Accumulated() == 1, "Mode switch must reset sample count");
                    draw();
                    Require(renderer.Accumulated() == 2, "Static frame must accumulate");
                    Require(!HasPass(renderer, "DXR.BuildBLAS"), "Static scene must not rebuild BLAS");
                    Require(HasPass(renderer, gi == 0   ? "GI.PathTracing"
                                              : gi == 1 ? "GI.SSGI"
                                                        : "ReSTIR.Evaluate"),
                            "Selected GI branch must execute");

                    // 仅相机版本变化应使辐射历史失效，而不上传或重建几何。
                    world.Registry().get<gameplay::Transform>(world.CameraEntity()).position.x += 0.01f;
                    world.MarkDirty();
                    draw();
                    Require(renderer.Accumulated() == 1, "World revisions must reset history without an editor flag");
                    Require(!HasPass(renderer, "Scene.Triangles.Upload"), "Camera motion must preserve geometry allocations");

                    // 隐藏或恢复几何范围会更新渲染表示；光源强度变化仅影响材质。
                    const auto meshes = world.Registry().view<gameplay::MeshRenderer>();
                    const auto entity = *meshes.begin();
                    auto &mesh = world.Registry().get<gameplay::MeshRenderer>(entity);
                    mesh.visible = false;
                    world.MarkDirty();
                    draw();
                    Require(HasPass(renderer, "Scene.Triangles.Upload"), "Visibility edits must upload geometry");
                    Require(HasPass(renderer, "DXR.BuildBLAS") == (backend == 1),
                            "Changed geometry rebuilds hardware acceleration only for the active DXR backend");
                    mesh.visible = true;
                    world.MarkDirty();
                    draw();
                    world.Registry().get<gameplay::Light>(world.LightEntity()).intensity += 0.1f;
                    world.MarkDirty();
                    draw();
                    Require(HasPass(renderer, "Scene.Materials.Upload"), "Emitter radiance edits must upload materials");
                    Require(!HasPass(renderer, "Scene.Triangles.Upload"), "Emitter radiance must not trigger geometry upload");

                    // 四种可见性生产方式必须兼容全部积分器和几何查询后端。
                    for (unsigned shadow = 0; shadow < 4; ++shadow) {
                        settings.shadowMode = shadow;
                        settings.reset = true;
                        std::cout << "Shadow matrix backend=" << backend << " GI=" << gi << " shadow=" << shadow << '\n';
                        draw();
                        CheckShadowGraph(renderer, shadow);
                        const auto image = readback.Read(renderer.Display());
                        Require(image.LitFraction() > 0.02, "Every GI/backend/shadow combination must produce a visible scene");
                    }
                    settings.shadowMode = 0;
                }
            }

            // 单点光没有随机提案维度，后续变化应来自阴影滤波。
            DirectOnly(settings);
            auto &primaryLight = world.Registry().get<gameplay::Light>(world.LightEntity());
            auto &primaryPose = world.Registry().get<gameplay::Transform>(world.LightEntity());
            primaryLight.type = gameplay::LightType::Point;
            primaryLight.intensity = 24;
            primaryLight.color = {1, 1, 1};
            primaryLight.sourceRadius = 0.3f;
            primaryPose.position = {-0.8f, 2.35f, -0.8f};
            world.MarkDirty();
            settings.shadowRadius = 2.5f;
            std::array<Image, 4> shadowImages;
            for (unsigned mode = 0; mode < 4; ++mode) {
                settings.shadowMode = mode;
                settings.reset = true;
                draw();
                CheckShadowGraph(renderer, mode);
                shadowImages[mode] = readback.Read(renderer.Display());
                Require(shadowImages[mode].LitFraction() > 0.02, "Filtered point-light shadows must not black out the scene");
            }
            settings.shadowMode = 1;
            settings.reset = true;
            draw();
            Require(Difference(shadowImages[1], readback.Read(renderer.Display())) < 1e-7,
                    "Repeated single-point direct rendering must be deterministic before filter images are compared");
            for (unsigned a = 1; a < 4; ++a)
                for (unsigned b = a + 1; b < 4; ++b) {
                    const double difference = Difference(shadowImages[a], shadowImages[b]);
                    std::cout << "Shadow image difference " << a << '/' << b << " = " << difference << '\n';
                    Require(difference > 0.00005, "PCF, PCSS and VSM must affect actual shaded pixels differently");
                }
            settings.shadows = false;
            settings.reset = true;
            draw();
            CheckShadowGraph(renderer, 1, false);
            Require(Difference(shadowImages[0], readback.Read(renderer.Display())) > 0.0005,
                    "Disabling visibility must change the occluded point-light image");

            // 单色开关比较检查房间下部，排除直接可见的天花板光源。
            for (unsigned type = 0; type < 5; ++type) {
                primaryLight.type = static_cast<gameplay::LightType>(type);
                primaryLight.enabled = false;
                primaryLight.direction = {0, -1, 0};
                primaryLight.tangent = {1, 0, 0};
                primaryLight.bitangent = {0, 0, 1};
                primaryLight.width = primaryLight.height = 0.8f;
                primaryLight.range = 12;
                primaryLight.intensity = type == 0 ? 1.5f : type == 2 || type == 3 ? 18.0f : 45.0f;
                primaryLight.color = {1, 0, 0};
                primaryPose.position = {-0.65f, 2.35f, -0.3f};
                primaryPose.rotationDegrees = {};
                settings.samples = type == 2 || type == 3 ? 16u : 1u;
                world.MarkDirty();
                draw();
                const auto disabled = readback.Read(renderer.Display());
                Require(disabled.LitFraction() < 0.001, "A scene without enabled lights or emission must render black");

                primaryLight.enabled = true;
                world.MarkDirty();
                draw();
                const auto red = readback.Read(renderer.Display());
                const auto redMean = red.Mean(red.height / 2);
                Require(redMean[0] > 0.005 && redMean[1] < 0.0001 && redMean[2] < 0.0001,
                        "Each light type must illuminate surfaces with its actual red spectrum");
                Require(Difference(red, disabled, red.height / 2) > 0.001,
                        "Disabling each source must remove its surface contribution");

                primaryLight.color = {0, 0, 1};
                world.MarkDirty();
                draw();
                const auto blue = readback.Read(renderer.Display());
                const auto blueMean = blue.Mean(blue.height / 2);
                Require(blueMean[2] > 0.005 && blueMean[0] < 0.0001 && blueMean[1] < 0.0001,
                        "Each light type must respond to an edited blue spectrum");
                Require(HasPass(renderer, "Scene.Lights.Upload"), "Light color edits must reach the uploaded GPU light table");

                // 面光源采样逐帧变化，先测量未修改对照，
                // 再判断差异是否来自姿态变化。
                draw();
                const double unchangedNoise = Difference(blue, readback.Read(renderer.Display()), blue.height / 2);

                // 移动面光、点光或旋转方向光、聚光，应改变受光表面。
                if (type == 0 || type == 4)
                    primaryPose.rotationDegrees.z = 55;
                else
                    primaryPose.position.x = 1.1f;
                world.MarkDirty();
                draw();
                const auto moved = readback.Read(renderer.Display());
                Require(Difference(blue, moved, blue.height / 2) > std::max(0.001, unchangedNoise * 2.5),
                        "Each light's edited pose must alter surface illumination beyond unchanged-frame sampling noise");
                std::cout << "Light type " << type << " red=" << redMean[0] << " blue=" << blueMean[2]
                          << " pose difference=" << Difference(blue, moved, blue.height / 2)
                          << " control noise=" << unchangedNoise << '\n';
            }

            // 读回分配跨图保留；捕获后缩放必须释放全部后备缓冲引用。
            capture.Capture(renderer.Display(), L"captures/integration-scene.png");
            const auto displayState = renderer.Display().state;
            swapchain.Resize(800, 600);
            draw();
            capture.Capture(renderer.Display(), L"captures/integration-scene-after-resize.png");
            Require(renderer.Display().state == displayState, "Capture must restore the display resource state");
            swapchain.Resize(320, 240);
            draw();
        }

        // 建立独立导入资产场景之前，先释放首个场景的大型阴影图集。
        device.WaitIdle();
        resources.Reset();
        {
            auto scene = ImportedFixture();
            const unsigned flatNormal = static_cast<unsigned>(scene.textureViews.size() - 2);
            const unsigned variedNormal = flatNormal + 1;
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120, std::move(scene));
            gameplay::World world;
            world.CreateScene(renderer.ReferenceScene());
            auto &camera = world.Registry().get<gameplay::Transform>(world.CameraEntity());
            camera.position = {0, 2.2f, -5};
            camera.rotationDegrees = {18, 0, 0};
            world.MarkDirty();
            render::RenderSettings settings;
            DirectOnly(settings);
            settings.shadows = false;
            ImageReadback readback(device, resources);
            const auto draw = [&] { Draw(renderer, world, settings, device, descriptors, swapchain); };
            const unsigned backendCount = device.DxrSupported() ? 2u : 1u;
            for (unsigned backend = 0; backend < backendCount; ++backend) {
                settings.backend = backend;
                settings.reset = true;
                draw();
                Require(readback.Read(renderer.Display()).LitFraction() > 0.02,
                        "Imported named meshes, textures and punctual lights must render on every backend");
            }
            Require(!renderer.ReferenceScene().sourcePath.empty(), "Integration must use the actual imported asset path");

            // 保留确定性的白色点光源，避免纹理或法线测试因随机光源
            // 选择变化而误判通过。
            auto lightView = world.Registry().view<gameplay::Light>();
            for (auto entity : lightView)
                world.Registry().get<gameplay::Light>(entity).enabled = entity == world.LightEntity();
            auto &light = world.Registry().get<gameplay::Light>(world.LightEntity());
            light.type = gameplay::LightType::Point;
            light.color = {1, 1, 1};
            light.intensity = 40;
            world.Registry().get<gameplay::Transform>(world.LightEntity()).position = {-1.5f, 2.5f, -2};
            auto &ground = world.Materials()[0];
            const auto baseTexture = ground.textures.x;
            // 新挂接法线贴图使用显式强度，不依赖 glTF 缺省 normalTexture 的默认值。
            ground.extras.y = 1;
            ground.textures.z = flatNormal;
            ground.textures.x = ~0u;
            world.MarkDirty();
            draw();
            const auto plain = readback.Read(renderer.Display());
            ground.textures.x = baseTexture;
            world.MarkDirty();
            draw();
            const auto textured = readback.Read(renderer.Display());
            Require(HasPass(renderer, "Scene.Materials.Upload") && !HasPass(renderer, "Scene.Triangles.Upload"),
                    "Texture-binding edits must update evaluated materials without rebuilding geometry");
            Require(Difference(plain, textured) > 0.005, "Imported UV base-color sampling must change actual image pixels");
            ground.textures.z = variedNormal;
            world.MarkDirty();
            draw();
            const auto mapped = readback.Read(renderer.Display());
            std::cout << "Imported UV difference=" << Difference(plain, textured)
                      << " normal-map difference=" << Difference(textured, mapped) << '\n';
            Require(Difference(textured, mapped) > 0.001, "UV normal-map samples must affect surface lighting");
            ground.extras.y = 0;
            world.MarkDirty();
            draw();
            Require(Difference(mapped, readback.Read(renderer.Display())) > 0.001,
                    "Normal strength zero must remove the sampled normal perturbation");

            // 闭合导入盒体覆盖真实粗糙透射路径，并通过重复对照估计
            // 蒙特卡洛噪声。
            settings.gi = 0;
            settings.pathVertices = 6;
            settings.samples = 16;
            settings.shadows = true;
            ground.extras.y = 1;
            ground.textures.z = flatNormal;
            auto &box = world.Materials()[1];
            box.baseRoughness = {0.9f, 0.95f, 1, 0.08f};
            box.emissionMetallic.w = 0;
            box.transmissionIor = {0.95f, 1.45f, 0, 0.5f};
            box.textures.x = ~0u;
            box.flags.y = 1;
            world.MarkDirty();
            const auto converge = [&] {
                settings.reset = true;
                for (unsigned frame = 0; frame < 8; ++frame)
                    draw();
                return readback.Read(renderer.Display());
            };
            const auto smoothGlass = converge();
            const auto smoothControl = converge();
            const double noise = Difference(smoothGlass, smoothControl);
            box.baseRoughness.w = 0.7f;
            world.MarkDirty();
            const auto roughGlass = converge();
            const double materialDifference = Difference(smoothGlass, roughGlass);
            std::cout << "Rough BTDF image difference=" << materialDifference << " repeated-material noise=" << noise << '\n';
            Require(smoothGlass.LitFraction() > 0.01 && roughGlass.LitFraction() > 0.01,
                    "Imported rough dielectric paths must remain nonblank");
            Require(materialDifference > std::max(0.001, noise * 1.5),
                    "Rough dielectric edits must change the image beyond repeated-render noise");
        }
        // 自发光网格没有解析提案，可见接收面仍应收集其光照及修改后的光谱。
        device.WaitIdle();
        resources.Reset();
        {
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120, EmissiveMeshFixture());
            gameplay::World world;
            world.CreateScene(renderer.ReferenceScene());
            auto &camera = world.Registry().get<gameplay::Transform>(world.CameraEntity());
            camera.position = {0, 1.4f, -4.6f};
            camera.rotationDegrees = {};
            for (auto entity : world.Registry().view<gameplay::Light>())
                world.Registry().get<gameplay::Light>(entity).enabled = false;
            world.MarkDirty();
            render::RenderSettings settings;
            settings.samples = 16;
            settings.candidates = 32;
            settings.pathVertices = 2;
            settings.bloom = 0;
            settings.mediumDensity = 0;
            settings.autoExposure = false;
            settings.exposure = -1;
            settings.shadowMode = 0;
            ImageReadback readback(device, resources);
            const auto draw = [&] { Draw(renderer, world, settings, device, descriptors, swapchain); };
            const auto converge = [&] {
                settings.reset = true;
                for (unsigned frame = 0; frame < 6; ++frame)
                    draw();
                return readback.Read(renderer.Display());
            };
            const unsigned backendCount = device.DxrSupported() ? 2u : 1u;
            bool gatheredAllSpectra = true;
            for (unsigned backend = 0; backend < backendCount; ++backend) {
                settings.backend = backend;
                for (unsigned gi : {0u, 2u, 1u}) {
                    settings.gi = gi;
                    world.Materials()[3].emissionMetallic = {};
                    world.MarkDirty();
                    const auto unlit = converge();
                    Require(unlit.Mean(unlit.height / 2)[0] < 1e-7,
                            "Emissive-only regression must have no analytic or background illumination");
                    for (unsigned channel : {0u, 1u}) {
                        world.Materials()[3].emissionMetallic = channel == 0 ? render::Float4{12, 0, 0, 0}
                                                                           : render::Float4{0, 12, 0, 0};
                        world.MarkDirty();
                        const auto lit = converge();
                        const auto mean = lit.Mean(lit.height / 2);
                        std::cout << "Mesh emission backend=" << backend << " GI=" << gi << " channel=" << channel
                                  << " receiver RGB=" << mean[0] << ',' << mean[1] << ',' << mean[2] << '\n';
                        // 下半画面排除直接可见天花板像素，光照必须先到达接收网格。
                        gatheredAllSpectra &= mean[channel] > 0.001 && mean[1u - channel] < 1e-7 && mean[2] < 1e-7 &&
                                              Difference(unlit, lit, lit.height / 2) > 0.0003;
                    }
                }
            }
            Require(gatheredAllSpectra,
                    "Every GI mode must gather red and green emission from an unregistered mesh light onto receiving pixels");
        }
        }
        // 即使经过 ReSTIR 复用和辐射重建，被墙完全遮挡的光源也应产生零原始 HDR 贡献。
        device.WaitIdle();
        resources.Reset();
        {
            auto source = EmissiveMeshFixture();
            source.materials[3].emissionMetallic = {};
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120, source);
            gameplay::World world;
            world.CreateScene(source);
            world.Registry().get<gameplay::Transform>(world.CameraEntity()).position = {0, 1.4f, -4.6f};
            auto &light = world.Registry().get<gameplay::Light>(world.LightEntity());
            light.type = gameplay::LightType::Point;
            light.color = {1, 1, 1};
            light.intensity = 30;
            light.emitterMaterial = ~0u;
            auto &lightPose = world.Registry().get<gameplay::Transform>(world.LightEntity());
            render::RenderSettings settings;
            settings.samples = 16;
            settings.candidates = 2;
            settings.pathVertices = 5;
            settings.shadowMode = 0;
            settings.bloom = 0;
            settings.mediumDensity = 0;
            ImageReadback readback(device, resources);
            const auto draw = [&] { Draw(renderer, world, settings, device, descriptors, swapchain); };
            const auto settle = [&] { for (unsigned frame = 0; frame < 32; ++frame) draw(); };
            const unsigned backendCount = device.DxrSupported() ? 2u : 1u;
            bool allOccluded = true;
            for (unsigned backend = 0; backend < backendCount; ++backend) {
                settings.backend = backend;
                for (unsigned gi : {0u, 2u, 1u}) {
                    settings.gi = gi;
                    for (const render::Vec3 outside : {render::Vec3{0, 1.8f, 3}, render::Vec3{0, -1, 0.4f}, render::Vec3{3, 1.8f, 0.4f}}) {
                        lightPose.position = outside;
                        world.MarkDirty();
                        settle();
                        const auto blocked = readback.Read(FrameImage(resources, "Resolve.Spatial"));
                        double maximum = 0;
                        std::size_t brightest = 0;
                        for (std::size_t index = 0; index < blocked.pixels.size(); ++index) {
                            Require(*std::min_element(blocked.pixels[index].begin(), blocked.pixels[index].end()) >= 0,
                                    "Raw HDR transport must remain nonnegative");
                            const auto value = *std::max_element(blocked.pixels[index].begin(), blocked.pixels[index].end());
                            if (value > maximum) { maximum = value; brightest = index; }
                        }
                        // 定点覆盖不允许把接收点放到房间源坐标的精确边界之外。
                        const auto positions = readback.Read(FrameImage(resources, "GBuffer.Position"));
                        for (const auto &position : positions.pixels)
                            Require(position[0] >= -1.4f && position[0] <= 1.4f && position[1] >= 0 && position[1] <= 2.8f &&
                                    position[2] >= -1.4f && position[2] <= 1.4f,
                                    "GBuffer receivers must stay within the Cornell source triangles at contact edges");
                        std::cout << "Opaque exterior HDR backend=" << backend << " GI=" << gi
                                  << " source=" << outside.x << ',' << outside.y << ',' << outside.z
                                  << " mean=" << blocked.Mean()[0] << " max=" << maximum << '\n';
                        if (maximum > 1e-8) {
                            const auto position = readback.Read(FrameImage(resources, "GBuffer.Position")).pixels[brightest];
                            const auto normal = readback.Read(FrameImage(resources, "GBuffer.Normal")).pixels[brightest];
                            const auto uv = readback.Read(FrameImage(resources, "GBuffer.UV")).pixels[brightest];
                            const auto direct = readback.Read(FrameImage(resources, "Lighting.Direct")).pixels[brightest];
                            std::cout << "Contact pixel " << brightest % blocked.width << ',' << brightest / blocked.width
                                      << " position=" << position[0] << ',' << position[1] << ',' << position[2]
                                      << " normal=" << normal[0] << ',' << normal[1] << ',' << normal[2]
                                      << " primitive+1=" << uv[2] << " direct=" << direct[0] << '\n';
                        }
                        allOccluded &= maximum < 1e-8;
                    }

                    // 把同一光源移到室内作为正对照，同时检查复用暗历史是否失效。
                    lightPose.position = {0, 2.4f, 0};
                    world.MarkDirty();
                    settle();
                    const auto lit = readback.Read(FrameImage(resources, "Resolve.Spatial"));
                    Require(lit.Mean()[0] > 0.02, "The blocked-light control must illuminate the same receiver after entering the room");

                    // 已有受光 reservoir 历史后恢复遮挡，旧重连不得残留光照。
                    lightPose.position = {0, 1.8f, 3};
                    world.MarkDirty();
                    draw();
                    const auto restored = readback.Read(FrameImage(resources, "Resolve.Spatial"));
                    for (const auto &pixel : restored.pixels) {
                        Require(*std::min_element(pixel.begin(), pixel.end()) >= 0, "Restored HDR transport must remain nonnegative");
                        allOccluded &= *std::max_element(pixel.begin(), pixel.end()) < 1e-8;
                    }
                    std::cout << "Opaque wall HDR backend=" << backend << " GI=" << gi
                              << " restored mean=" << restored.Mean()[0] << " visible mean=" << lit.Mean()[0] << '\n';
                }
            }
            Require(allOccluded, "Opaque walls/floor must block exterior radiance and clear illuminated histories on the first restored frame");
        }
        if (contactOnly) return 0;
        // 复制真实资源实例应扩展几何和属性上传，并在 DXR 模式更新加速结构。
        device.WaitIdle();
        resources.Reset();
        {
            render::Scene source;
            render::GpuMaterial material;
            material.baseRoughness = {0.5f, 0.5f, 0.5f, 1};
            material.emissionMetallic = {1, 0.2f, 0.1f, 0};
            source.materials.push_back(material);
            for (unsigned index = 0; index < 1100; ++index) {
                const float x = -1.0f + static_cast<float>(index % 55) / 55;
                const float y = -0.5f + static_cast<float>(index / 55) / 20;
                source.triangles.push_back({{x, y, 0, 0}, {x, y + 0.05f, 0, static_cast<float>(index + 1)},
                    {x + 1.0f / 55, y, 0, 0}});
                render::GpuTriangleAttributes attribute;
                attribute.normal0 = attribute.normal1 = attribute.normal2 = {0, 0, -1, 1};
                attribute.tangent = {1, 0, 0, 1};
                source.attributes.push_back(attribute);
            }
            source.meshParts = {{"Shared grid", 0, 1100, 0}};
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120, source);
            gameplay::World world;
            world.CreateScene(source);
            world.Registry().get<gameplay::Transform>(world.CameraEntity()).position = {0, 0, -4.6f};
            world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
            world.MarkDirty();
            const auto original = *world.Registry().view<gameplay::MeshRenderer>().begin();
            gameplay::Entity selected = original;
            render::RenderSettings settings;
            DirectOnly(settings);
            settings.shadows = false;
            ImageReadback readback(device, resources);
            const auto draw = [&] { Draw(renderer, world, settings, device, descriptors, swapchain); };
            const unsigned backendCount = device.DxrSupported() ? 2u : 1u;
            for (unsigned backend = 0; backend < backendCount; ++backend) {
                settings.backend = backend;
                draw();
                const auto baseline = readback.Read(renderer.Display());
                const auto before = render::editor::CaptureEditorDocument(world, settings, selected);
                selected = world.CreateEntity("Grid Copy");
                world.Registry().emplace<gameplay::MeshRenderer>(selected, world.Registry().get<gameplay::MeshRenderer>(original));
                world.Registry().get<gameplay::Transform>(selected).position.x = 1.2f;
                world.MarkDirty();
                render::editor::EditorHistory history;
                history.Record(before, render::editor::CaptureEditorDocument(world, settings, selected), "Duplicate");
                draw();
                const auto duplicated = readback.Read(renderer.Display());
                Require(Difference(baseline, duplicated) > 0.001, "A shared mesh duplicate must reach real GPU raster shading after capacity growth");
                Require(history.Undo(world, settings, selected), "Rendered duplicate creation undoes");
                draw();
                Require(Difference(baseline, readback.Read(renderer.Display())) < 1e-7, "Undo restores the deterministic original rendered geometry");
                Require(history.Redo(world, settings, selected), "Rendered duplicate creation redoes");
                draw();
                Require(Difference(duplicated, readback.Read(renderer.Display())) < 1e-7, "Redo restores the deterministic duplicate image on each backend");
                Require(history.Undo(world, settings, selected), "Next backend begins with original geometry");
            }
        }
        std::cout << "PASS GPU integration: all available backends, 3 GI, 4 shadow modes, five light types, imported "
                     "UV/normal/rough BTDF, emissive mesh transport, opaque contact visibility, duplicate capacity/undo/redo, revisions and resize\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL GPU integration: " << error.what() << '\n';
        return 1;
    }
}
