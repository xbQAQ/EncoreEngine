// 光栅验证从帧入口执行完整管线，避免直接耦合算法实现文件。
#include "Frame/FrameRenderer.h"
#include "asset/GltfImporter.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifndef RENDER_SOURCE_DIRECTORY
#define RENDER_SOURCE_DIRECTORY L"."
#endif

namespace {
void Require(bool condition, const char* message) {
    // Release GPU 测试仍须保留所有图像与通道图断言。
    if (!condition) throw std::runtime_error(message);
}

struct Image {
    std::vector<std::array<double, 3>> pixels; // 从实际光栅渲染结果回读的显示 RGB。
    double LitFraction() const {
        // 排除数值黑色，避免仅因 alpha 已初始化就认定图像有效。
        size_t count = 0;
        for (const auto& pixel : pixels)
            if (*std::max_element(pixel.begin(), pixel.end()) > 0.01) ++count;
        return double(count) / pixels.size();
    }
};

double Difference(const Image& a, const Image& b) {
    // 所有比较状态使用相同的固定视口与曝光。
    Require(a.pixels.size() == b.pixels.size(), "Raster image dimensions must match");
    double error = 0;
    for (size_t index = 0; index < a.pixels.size(); ++index)
        for (unsigned channel = 0; channel < 3; ++channel)
            error += std::abs(a.pixels[index][channel] - b.pixels[index][channel]);
    return error / (a.pixels.size() * 3);
}

class Readback {
public:
    Readback(render::dx12::Dx12Device& device, render::dx12::ResourcePool& resources)
        : device_(device), resources_(resources) {}
    Image Read(render::dx12::GpuResource& source) {
        // 独立图执行真实 COPY_SOURCE 回读，并恢复场景图像状态。
        const auto description = source.object->GetDesc();
        Require(description.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Raster display readback requires RGBA8");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes{};
        device_.Native()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        if (!buffer_) buffer_ = &resources_.Buffer("RasterTest.Readback", bytes, 0, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
        const auto previousState = source.state;
        device_.BeginCommands();
        render::RenderGraph graph;
        render::dx12::GraphExecutor executor(device_);
        executor.Import(graph, resources_);
        graph.AddPass("RasterTest.Readback", {render::dx12::Read(source, D3D12_RESOURCE_STATE_COPY_SOURCE),
            render::dx12::Write(*buffer_, D3D12_RESOURCE_STATE_COPY_DEST)}, [&] {
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = source.object.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = buffer_->object.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = footprint;
            device_.Commands()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }, true);
        graph.Export(source.graphId, previousState);
        executor.Execute(graph);
        device_.SubmitAndWait();

        // 只解码真实图像宽度，D3D12 行填充不属于图像内容。
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        render::dx12::Check(buffer_->object->Map(0, &range, &mapped), "Map raster display readback");
        const auto* data = static_cast<const unsigned char*>(mapped);
        Image result;
        result.pixels.resize(size_t(description.Width) * description.Height);
        for (unsigned y = 0; y < description.Height; ++y)
            for (unsigned x = 0; x < description.Width; ++x)
                for (unsigned channel = 0; channel < 3; ++channel)
                    result.pixels[size_t(y) * description.Width + x][channel] =
                        data[size_t(y) * footprint.Footprint.RowPitch + size_t(x) * 4 + channel] / 255.0;
        const D3D12_RANGE written{0, 0};
        buffer_->object->Unmap(0, &written);
        return result;
    }
private:
    render::dx12::Dx12Device& device_; // 独立图像回读图使用的队列所有者。
    render::dx12::ResourcePool& resources_; // 固定尺寸测试的持久回读分配所有者。
    render::dx12::GpuResource* buffer_{}; // 栅栏完成后可复用的暂存缓冲。
};

bool HasPass(const render::FrameRenderer& renderer, const char* name) {
    // 检查编译后保留顺序，验证设置确实作用于渲染器。
    const auto& passes = renderer.PassNames();
    return std::find(passes.begin(), passes.end(), name) != passes.end();
}

void VerifyRasterGraph(const render::FrameRenderer& renderer, unsigned shadowMode, bool shadows) {
    // 纯光栅模式不得调度加速结构构建或基于光线的 GI 算法。
    Require(HasPass(renderer, "Raster.GBuffer") && HasPass(renderer, "Lighting.Direct") && HasPass(renderer, "GI.SSGI"),
        "Raster mode must execute deferred geometry, direct lighting and screen-space GI");
    for (const auto& pass : renderer.PassNames())
        Require(!pass.starts_with("DXR.") && !pass.starts_with("ReSTIR.") && pass != "GI.PathTracing",
            "Pure raster mode must not schedule DXR builds, path tracing or ReSTIR passes");
    Require(HasPass(renderer, "Shadow.RasterDepth") == shadows, "Raster visibility must use shadow maps when enabled");
    Require(HasPass(renderer, "Shadow.BlurHorizontal") == (shadows && shadowMode == 3),
        "Only VSM may schedule horizontal moment filtering");
    Require(HasPass(renderer, "Shadow.BlurVertical") == (shadows && shadowMode == 3),
        "Only VSM may schedule vertical moment filtering");
    Require(renderer.Profile().gpuAvailable, "Raster frames must publish real completed GPU pass timings");
}

render::RenderSettings RasterSettings() {
    // 故意请求光线功能，验证管线选择仍将实际帧约束为纯光栅。
    render::RenderSettings settings;
    settings.pipeline = 0;
    settings.backend = 1;
    settings.gi = 2;
    settings.samples = 8;
    settings.temporalReuse = settings.spatialReuse = false;
    settings.bloom = 0;
    settings.autoExposure = false;
    settings.exposure = -1;
    settings.giIntensity = 0;
    settings.shadowMode = 2;
    settings.shadowRadius = 2.5f;
    return settings;
}
}

int main() {
    try {
        // 无需窗口或交换链，直接验证图导出的实际 Display 产品。
        render::dx12::Dx12Device device(true);
        render::dx12::DescriptorArena descriptors(device);
        render::dx12::ResourcePool resources(device, descriptors);
        {
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120);
            auto& backbuffer = resources.Texture("RasterTest.Backbuffer", 160, 120,
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
            gameplay::World world;
            world.CreateCornell(renderer.ReferenceScene());
            auto settings = RasterSettings();
            Readback readback(device, resources);
            const auto draw = [&] {
                renderer.Draw(world.ExtractRenderSnapshot(), settings, 1.0f / 60, backbuffer, [](render::RenderGraph&) {});
                VerifyRasterGraph(renderer, settings.shadowMode == 0 ? 2 : settings.shadowMode, settings.shadows);
            };
            // 默认空闲帧不得发布性能样本；只有显式开启录制才插入时间戳和逐通道 CPU 计时。
            renderer.Draw(world.ExtractRenderSnapshot(), settings, 1.0f / 60, backbuffer, [](render::RenderGraph&) {});
            Require(renderer.Profile().frameIndex == 0 && renderer.Profile().passes.empty() && !renderer.Profile().gpuAvailable,
                "Rendering without a recording request must not collect CPU or GPU profiles");
            renderer.SetProfilingEnabled(true);
            draw();
            Require(readback.Read(renderer.Display()).LitFraction() > 0.02, "Cornell pure raster output must not be blank");

            // 停止后连续渲染不能覆盖最后的完成样本，重新录制才继续生成新的采集序号。
            const auto recordedIndex = renderer.Profile().frameIndex;
            const auto recordedCpu = renderer.Profile().cpuMilliseconds;
            const auto recordedGpu = renderer.Profile().gpuMilliseconds;
            renderer.SetProfilingEnabled(false);
            draw(); draw();
            Require(renderer.Profile().frameIndex == recordedIndex && renderer.Profile().cpuMilliseconds == recordedCpu &&
                renderer.Profile().gpuMilliseconds == recordedGpu,
                "Stopping recording must preserve all last-frame timings across later renders");
            renderer.SetProfilingEnabled(true);
            draw();
            Require(renderer.Profile().frameIndex > recordedIndex, "Restarting recording must produce a fresh completed GPU sample");

            // 矩形 LTC 直接光不依赖帧随机种子或 SPP，禁用间接与介质后连续帧必须逐像素稳定。
            settings.samples = 1;
            settings.reset = true;
            draw();
            const auto areaLight = readback.Read(renderer.Display());
            for (unsigned frame = 0; frame < 4; ++frame) draw();
            Require(Difference(areaLight, readback.Read(renderer.Display())) == 0,
                "Raster area lighting must remain exactly stable across frame indices");
            settings.samples = 16;
            settings.reset = true;
            draw();
            Require(Difference(areaLight, readback.Read(renderer.Display())) == 0,
                "Path sampling budget must not change deterministic raster direct illumination");

            // 确定性点光源使过滤器差异不受面光源采样噪声干扰。
            auto& light = world.Registry().get<gameplay::Light>(world.LightEntity());
            light.type = gameplay::LightType::Point;
            light.color = {1, 1, 1};
            light.intensity = 24;
            light.sourceRadius = 0.3f;
            world.Registry().get<gameplay::Transform>(world.LightEntity()).position = {-0.8f, 2.35f, -0.8f};
            world.MarkDirty();
            std::array<Image, 3> filters;
            for (unsigned mode = 1; mode <= 3; ++mode) {
                settings.shadowMode = mode;
                settings.reset = true;
                draw();
                filters[mode - 1] = readback.Read(renderer.Display());
                Require(filters[mode - 1].LitFraction() > 0.02, "Every raster shadow filter must retain visible illumination");
            }
            for (unsigned a = 0; a < 3; ++a)
                for (unsigned b = a + 1; b < 3; ++b)
                    Require(Difference(filters[a], filters[b]) > 0.00005,
                        "PCF, PCSS and VSM must change actual pure raster shaded pixels");
            settings.shadowMode = 0;
            settings.reset = true;
            draw();
            Require(Difference(filters[1], readback.Read(renderer.Display())) < 0.000001,
                "The ray-shadow selection must normalize to PCSS in the pure raster pipeline");

            // 屏幕空间 GI 必须贡献真实反弹辐亮度，离屏命中缺失时不能回退光线。
            settings.shadowMode = 2;
            settings.giIntensity = 3;
            settings.samples = 16;
            settings.reset = true;
            draw();
            const auto indirect = readback.Read(renderer.Display());
            Require(Difference(filters[1], indirect) > 0.0005, "Screen-space GI must contribute to the Cornell raster image");
            settings.giIntensity = 0;
            settings.mediumDensity = 0.04f;
            settings.reset = true;
            draw();
            const auto volume = readback.Read(renderer.Display());
            Require(volume.LitFraction() > 0.02 && Difference(filters[1], volume) > 0.0001,
                "Pure raster volumetric light must use shadow-map visibility and affect the output");
            std::cout << "Raster Cornell: GI difference " << Difference(filters[1], indirect)
                << ", volume difference " << Difference(filters[1], volume) << '\n';
        }

        // 真实 glTF 导入验证相同光栅排列中的纹理材质编辑。
        device.WaitIdle();
        resources.Reset();
        {
            auto scene = render::ImportGltf(std::filesystem::path(RENDER_SOURCE_DIRECTORY) / L"Tests/fixtures/textured_multilight.gltf");
            render::FrameRenderer renderer(device, descriptors, resources, 160, 120, std::move(scene));
            // 此测试显式测量导入场景，避免依赖生产渲染器默认开启采集。
            renderer.SetProfilingEnabled(true);
            auto& backbuffer = resources.Texture("RasterTest.ImportBackbuffer", 160, 120,
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
            gameplay::World world;
            world.CreateScene(renderer.ReferenceScene());
            auto& camera = world.Registry().get<gameplay::Transform>(world.CameraEntity());
            camera.position = {0, 2.2f, -5};
            camera.rotationDegrees = {18, 0, 0};
            for (auto entity : world.Registry().view<gameplay::Light>())
                world.Registry().get<gameplay::Light>(entity).enabled = entity == world.LightEntity();
            auto& light = world.Registry().get<gameplay::Light>(world.LightEntity());
            light.type = gameplay::LightType::Point;
            light.color = {1, 1, 1};
            light.intensity = 40;
            world.Registry().get<gameplay::Transform>(world.LightEntity()).position = {-1.5f, 2.5f, -2};
            auto settings = RasterSettings();
            settings.shadows = false;
            Readback readback(device, resources);
            const auto draw = [&] {
                renderer.Draw(world.ExtractRenderSnapshot(), settings, 1.0f / 60, backbuffer, [](render::RenderGraph&) {});
                VerifyRasterGraph(renderer, settings.shadowMode, false);
            };
            auto& ground = world.Materials()[0];
            const auto texture = ground.textures.x;
            ground.textures.x = ~0u;
            world.MarkDirty();
            draw();
            const auto plain = readback.Read(renderer.Display());
            ground.textures.x = texture;
            world.MarkDirty();
            draw();
            const auto textured = readback.Read(renderer.Display());
            Require(textured.LitFraction() > 0.02, "Imported textured model must render in the pure raster pipeline");
            Require(HasPass(renderer, "Scene.Materials.Upload") && !HasPass(renderer, "Scene.Triangles.Upload"),
                "Imported material edits must upload materials without geometry or acceleration rebuilds");
            Require(Difference(plain, textured) > 0.005, "Imported UV material changes must affect real raster pixels");
            std::cout << "Raster imported material difference " << Difference(plain, textured) << '\n';
        }
        std::cout << "Pure raster pipeline GPU tests passed on " << device.AdapterName() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
