// 集成测试通过帧宿主驱动效果，不向算法模块增加测试接口。
#include "Frame/FrameRenderer.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
using Pixel = std::array<double, 4>;
using Image = std::vector<Pixel>;

double Difference(const Image& a, const Image& b) {
    Require(a.size() == b.size(), "Effect image dimensions must match");
    double sum = 0;
    for (std::size_t index = 0; index < a.size(); ++index)
        for (unsigned channel = 0; channel < 3; ++channel) sum += std::abs(a[index][channel] - b[index][channel]);
    return sum / (3 * a.size());
}
double Brightness(const Image& image) {
    double sum = 0;
    for (const auto& pixel : image) sum += pixel[0] + pixel[1] + pixel[2];
    return sum / (3 * image.size());
}
double GeometryDifference(const Image& a, const Image& b, const Image& positions) {
    // 背景天空本身不能证明积分器使用了未命中辐亮度，只比较真实表面覆盖。
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (positions[index][3] <= 0) continue;
        for (unsigned channel = 0; channel < 3; ++channel) sum += std::abs(a[index][channel] - b[index][channel]);
        ++count;
    }
    Require(count > 0, "Sky transport test requires actual covered geometry");
    return sum / (3 * count);
}
bool HasPass(const render::FrameRenderer& renderer, const char* name) {
    const auto& passes = renderer.PassNames();
    return std::find(passes.begin(), passes.end(), name) != passes.end();
}

class DisplayReadback {
public:
    DisplayReadback(render::dx12::Dx12Device& device, render::dx12::ResourcePool& pool) : device_(device), pool_(pool) {}
    Image Read(render::dx12::GpuResource& source) {
        const auto description = source.object->GetDesc();
        Require(description.Format == DXGI_FORMAT_R8G8B8A8_UNORM || description.Format == DXGI_FORMAT_R32G32B32A32_FLOAT,
            "Effects readback supports display RGBA8 or geometry RGBA32F");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes{};
        device_.Native()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        if (bytes > capacity_) {
            buffer_ = &pool_.Buffer("EffectsTest.Readback", bytes, 0, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
            capacity_ = bytes;
        }
        // 实际 GPU 拷贝完成后才解码 CPU 图像，不能用 Pass 名称代替效果验证。
        device_.BeginCommands();
        render::RenderGraph graph;
        render::dx12::GraphExecutor executor(device_);
        executor.Import(graph, pool_);
        const auto previousState = source.state;
        graph.AddPass("EffectsTest.Readback", {render::dx12::Read(source, D3D12_RESOURCE_STATE_COPY_SOURCE),
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
        void* mapped{};
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
        render::dx12::Check(buffer_->object->Map(0, &readRange, &mapped), "Map screen effects readback");
        Image result(static_cast<std::size_t>(description.Width) * description.Height);
        const auto* pixels = static_cast<const unsigned char*>(mapped);
        for (unsigned y = 0; y < description.Height; ++y)
            for (unsigned x = 0; x < description.Width; ++x)
                for (unsigned channel = 0; channel < 4; ++channel) {
                    const auto* row = pixels + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
                    result[static_cast<std::size_t>(y) * description.Width + x][channel] =
                        description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? row[x * 4 + channel] / 255.0 :
                        static_cast<double>(reinterpret_cast<const float*>(row)[x * 4 + channel]);
                }
        const D3D12_RANGE written{0, 0};
        buffer_->object->Unmap(0, &written);
        return result;
    }
private:
    render::dx12::Dx12Device& device_; // 真实命令与栅栏完成状态的所有者。
    render::dx12::ResourcePool& pool_; // 固定尺寸回读缓冲的长期所有者。
    render::dx12::GpuResource* buffer_{}; // 重复回读复用同一原生缓冲。
    UINT64 capacity_{}; // 显示及浮点位置回读所需的最大缓冲容量。
};

void ValidateEffects() {
    render::dx12::Dx12Device device(true);
    render::dx12::DescriptorArena descriptors(device);
    render::dx12::ResourcePool pool(device, descriptors);
    render::FrameRenderer renderer(device, descriptors, pool, 160, 120);
    renderer.RetainFrameImages();
    auto& backbuffer = pool.Texture("EffectsTest.Backbuffer", 160, 120, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    gameplay::World world;
    world.CreateCornell(renderer.ReferenceScene());
    render::RenderSettings settings;
    settings.pipeline = 0;
    settings.samples = 16;
    settings.giIntensity = 0;
    settings.temporalReuse = settings.spatialReuse = false;
    settings.shadows = false;
    settings.bloom = 0;
    settings.exposure = -1;
    settings.autoExposure = false;
    auto& light = world.Registry().get<gameplay::Light>(world.LightEntity());
    light.type = gameplay::LightType::Point;
    light.color = {1, 1, 1};
    light.intensity = 24;
    world.Registry().get<gameplay::Transform>(world.LightEntity()).position = {-0.8f, 2.35f, -0.8f};
    world.MarkDirty();
    DisplayReadback readback(device, pool);
    const auto draw = [&] {
        settings.reset = true;
        renderer.Draw(world.ExtractRenderSnapshot(), settings, 1.0f / 60, backbuffer, [](render::RenderGraph&) {});
        if (settings.pipeline == 0)
            for (const auto& pass : renderer.PassNames())
                Require(!pass.starts_with("DXR.") && !pass.starts_with("ReSTIR.") && pass != "GI.PathTracing",
                    "Screen effects in raster mode may not dispatch scene ray queries");
        return readback.Read(renderer.Display());
    };

    // 所有新开关默认关闭，关闭时图中没有对应效果或额外 native 分配。
    const auto baseline = draw();
    for (const auto* pass : {"Effects.SSAO", "Effects.SSR", "Effects.SSIL", "Effects.SSS.Horizontal", "Effects.SSS.Vertical", "Sky.Background"})
        Require(!HasPass(renderer, pass), "Disabled effects must not schedule production passes");
    Require(Brightness(baseline) > 0.005, "Effects test baseline must contain actual shaded geometry");

    // AO 在接触面附近只能降低漫反射显示能量，零强度应逐像素恢复。
    settings.ssao = true;
    settings.ssaoRadius = 1;
    settings.ssaoIntensity = 4;
    const auto occluded = draw();
    Require(HasPass(renderer, "Effects.SSAO") && Difference(baseline, occluded) > 0.00001 &&
        Brightness(occluded) < Brightness(baseline), "SSAO must darken real contact pixels");
    settings.ssaoIntensity = 0;
    Require(Difference(baseline, draw()) < 0.000001, "Zero SSAO strength must preserve the input image");
    settings.ssao = false;

    // 光泽表面的反射必须来自屏幕颜色追踪；天空关闭以免只验证环境兜底。
    for (auto& material : world.Materials()) {
        material.baseRoughness.w = 0.12f;
        material.emissionMetallic.w = 0.75f;
    }
    world.MarkDirty();
    const auto glossy = draw();
    settings.ssr = true;
    settings.ssrIntensity = 2;
    settings.ssrDistance = 8;
    const auto reflected = draw();
    Require(HasPass(renderer, "Effects.SSR") && Difference(glossy, reflected) > 0.00001,
        "SSR must change actual glossy pixels without sky fallback");
    settings.ssrIntensity = 0;
    Require(Difference(glossy, draw()) < 0.000001, "Zero SSR strength must preserve the input image");
    settings.ssr = false;

    // 独立 SSIL 在旧 GI 增益为零时仍能贡献一次漫反射，避免只是重复 SSGI 开关。
    for (auto& material : world.Materials()) {
        material.baseRoughness.w = 0.8f;
        material.emissionMetallic.w = 0;
        material.principled.w = 0;
    }
    world.MarkDirty();
    const auto diffuse = draw();
    settings.ssil = true;
    settings.ssilIntensity = 2;
    const auto indirect = draw();
    Require(HasPass(renderer, "Effects.SSIL") && Difference(diffuse, indirect) > 0.00001 &&
        Brightness(indirect) > Brightness(diffuse), "Independent SSIL must add real screen-space bounce light");
    // 实际 GI 历史必须为零，证明 SSIL 没有在旧 SSGI 上重复叠加同一反弹。
    const auto giHistory = std::find_if(pool.All().begin(), pool.All().end(), [](const auto& resource) {
        return resource->name == "History.GI";
    });
    Require(giHistory != pool.All().end() && Brightness(readback.Read(**giHistory)) < 0.0000001,
        "SSIL must suppress the duplicate SSGI energy component in actual GPU history");
    settings.ssilIntensity = 0;
    Require(Difference(diffuse, draw()) < 0.000001, "Zero SSIL strength must preserve SSGI and the input image");
    settings.ssil = false;

    // 未标记材质完全不扩散；Disney subsurface 非零后才出现双向扩散。
    settings.sss = true;
    settings.sssRadius = 0.25f;
    settings.sssIntensity = 1;
    Require(Difference(diffuse, draw()) < 0.000001, "Unmarked materials may not receive screen-space SSS");
    settings.sss = false;
    for (auto& material : world.Materials()) material.principled.w = 1;
    world.MarkDirty();
    const auto marked = draw();
    settings.sss = true;
    const auto scattered = draw();
    Require(HasPass(renderer, "Effects.SSS.Horizontal") && HasPass(renderer, "Effects.SSS.Vertical") &&
        Difference(marked, scattered) > 0.00001, "Marked subsurface material must diffuse actual shaded pixels");
    settings.sss = false;

    // 移除全部有限发光源，避免随机直接光噪声冒充天空传输差异。
    for (auto entity : world.Registry().view<gameplay::Light>()) world.Registry().get<gameplay::Light>(entity).enabled = false;
    for (auto& material : world.Materials()) {
        material.emissionMetallic.x = material.emissionMetallic.y = material.emissionMetallic.z = 0;
    }
    world.MarkDirty();
    // 天空填充背景，并通过真实 PT/ReSTIR 未命中路径照亮表面。
    const auto noSky = draw();
    settings.sky = true;
    settings.skyIntensity = 1;
    const auto sky = draw();
    Require(HasPass(renderer, "Sky.Background") && Difference(noSky, sky) > 0.001,
        "Sky background must change actual raster background pixels");
    const auto position = std::find_if(pool.All().begin(), pool.All().end(), [](const auto& resource) {
        return resource->name == "GBuffer.Position";
    });
    Require(position != pool.All().end(), "Sky transport coverage requires the explicit geometry snapshot");
    const auto positions = readback.Read(**position);
    for (unsigned backend : {0u, 1u}) {
        if (backend == 1 && !device.DxrSupported()) continue;
        settings.pipeline = 1;
        settings.backend = backend;
        settings.giIntensity = 1;
        for (unsigned gi : {0u, 2u}) {
            settings.gi = gi;
            settings.sky = false;
            const auto dark = draw();
            settings.sky = true;
            const auto illuminated = draw();
            const auto geometryDifference = GeometryDifference(dark, illuminated, positions);
            Require(geometryDifference > 0.00001, "Sky BSDF misses must illuminate covered PT and ReSTIR surfaces");
            std::cout << "Sky geometry backend " << backend << " GI " << gi << ": " << geometryDifference << '\n';
        }
    }
    std::cout << "SSAO " << Difference(baseline, occluded) << ", SSR " << Difference(glossy, reflected)
        << ", SSIL " << Difference(diffuse, indirect) << ", SSS " << Difference(marked, scattered)
        << ", sky " << Difference(noSky, sky) << '\n';

    // 实时材质重载后的完整候选集合也必须包含全部新增效果程序。
    Require(renderer.ReloadMaterials({}).success, "Material reload must retain every effects pipeline");
    settings.pipeline = 0;
    settings.ssao = settings.ssr = settings.ssil = settings.sss = true;
    settings.ssaoIntensity = settings.ssrIntensity = settings.ssilIntensity = 1;
    Require(Brightness(draw()) > 0.005, "Combined effects after material reload must produce a valid display");
}
}

int main() {
    try {
        ValidateEffects();
        std::cout << "Screen-space effects GPU tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
