// 分别验证材质编译的管线缓存边界和实际帧输出。
#include "Pipeline/PipelineCache.h"
#include "dx12/Dx12Device.h"
#include "dx12/DescriptorArena.h"
#include "dx12/ResourcePool.h"
#include "dx12/GraphExecutor.h"
#include "Frame/FrameRenderer.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <vector>
#include <thread>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<render::Float4> ReadBase(render::dx12::Dx12Device& device, render::dx12::ResourcePool& resources) {
    render::dx12::GpuResource* base{};
    for (const auto& resource : resources.All()) if (resource->name == "GBuffer.BaseColor") base = resource.get();
    Check(base != nullptr, "retained base-color diagnostic is available");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    const auto description = base->object->GetDesc();
    device.Native()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto& target = resources.Buffer("MaterialTest.Readback", bytes, 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    device.BeginCommands();
    render::RenderGraph graph;
    render::dx12::GraphExecutor executor(device);
    executor.Import(graph, resources);
    // 数值读回使用正式渲染图执行器，并恢复诊断资源原来的精确状态。
    const auto original = base->state;
    graph.AddPass("MaterialTest.ReadBase", {render::dx12::Read(*base, D3D12_RESOURCE_STATE_COPY_SOURCE),
        render::dx12::Write(target, D3D12_RESOURCE_STATE_COPY_DEST)}, [&] {
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = base->object.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = target.object.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
        device.Commands()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    });
    graph.Export(base->graphId, original); graph.Export(target.graphId, D3D12_RESOURCE_STATE_COPY_DEST);
    executor.Execute(graph); device.SubmitAndWait();
    std::vector<render::Float4> result(static_cast<std::size_t>(description.Width) * description.Height);
    void* mapped{}; const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    render::dx12::Check(target.object->Map(0, &range, &mapped), "Map material test readback");
    for (unsigned row = 0; row < description.Height; ++row)
        std::memcpy(result.data() + row * description.Width,
            static_cast<const std::byte*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
            static_cast<std::size_t>(description.Width) * sizeof(render::Float4));
    const D3D12_RANGE written{0, 0}; target.object->Unmap(0, &written);
    return result;
}
}
int main() {
    try {
        render::dx12::Dx12Device device(false);
        render::PipelineCache pipelines(device);
        const auto* initial = pipelines.GBuffer();
        // 源文件携带 BOM 时仍应生成合法的运行时头文件，并保留材质的覆盖规则。
        const render::MaterialShaderSource valid{0,
            "\xef\xbb\xbf" "void Surface(inout Material material, float2 uv, float4 tangent) { material.baseRoughness.xyz=float3(0.1,0.7,0.2); material.flags.z=1; material.extras.w=0; }", "Test.hlsl"};
        const auto compiled = pipelines.ReloadMaterials(std::span(&valid, 1));
        Check(compiled.success, compiled.diagnostics.c_str());
        Check(pipelines.MaterialRevision() == 1 && pipelines.GBuffer() != initial, "valid source publishes new PSOs");
        const auto* lastGood = pipelines.GBuffer();
        auto invalid = valid;
        invalid.source = "void Surface(inout Material material, float2 uv, float4 tangent) { invalid_identifier = ; }";
        const auto rejected = pipelines.ReloadMaterials(std::span(&invalid, 1));
        Check(!rejected.success && !rejected.diagnostics.empty(), "invalid HLSL reports native compiler error");
        Check(pipelines.GBuffer() == lastGood && pipelines.MaterialRevision() == 1, "failed source preserves every last-good pipeline");
        Check(pipelines.Compute(L"DirectLighting", 2) != nullptr, "pure raster remains available after reload");
        Check(pipelines.ReloadMaterials({}).success && pipelines.MaterialRevision() == 2, "removing scripts restores built-in material evaluation");
        render::dx12::DescriptorArena descriptors(device);
        render::dx12::ResourcePool resources(device, descriptors);
        render::FrameRenderer renderer(device, descriptors, resources, 64, 64);
        auto& backbuffer = resources.Texture("MaterialTest.Backbuffer", 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        gameplay::World world;
        world.CreateCornell(renderer.ReferenceScene());
        render::RenderSettings settings;
        settings.pipeline = 0; settings.giIntensity = 0; settings.bloom = 0;
        const auto draw = [&] {
            renderer.RetainFrameImages();
            renderer.Draw(world.ExtractRenderSnapshot(), settings, 1.0f / 60, backbuffer, [](render::RenderGraph&) {});
        };
        draw(); const auto before = ReadBase(device, resources);
        const auto requested = std::chrono::steady_clock::now();
        renderer.RequestMaterialReload(std::span(&valid, 1));
        Check(std::chrono::steady_clock::now() - requested < std::chrono::milliseconds(500), "async compile request returns without blocking editor frame");
        draw();
        const auto whileCompiling = ReadBase(device, resources);
        Check(std::memcmp(before.data(), whileCompiling.data(), before.size() * sizeof(render::Float4)) == 0,
            "renderer continues drawing the last-good material while DXC runs");
        bool published = false;
        while (renderer.MaterialCompilationPending()) {
            if (const auto result = renderer.PollMaterialReload()) {
                Check(result->success, result->diagnostics.c_str()); published = true;
            }
            Check(std::chrono::steady_clock::now() - requested < std::chrono::seconds(90), "async runtime compilation finishes within test timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(published, "same renderer atomically publishes completed async surface program");
        settings.reset = true; draw(); const auto after = ReadBase(device, resources);
        std::size_t changed{}, expectedColor{};
        for (std::size_t index = 0; index < before.size(); ++index) {
            if (std::abs(before[index].x - after[index].x) + std::abs(before[index].y - after[index].y) +
                std::abs(before[index].z - after[index].z) > 0.05f) ++changed;
            if (std::abs(after[index].x - 0.1f) + std::abs(after[index].y - 0.7f) + std::abs(after[index].z - 0.2f) < 0.001f) ++expectedColor;
        }
        Check(changed > 100, "runtime HLSL visibly changes existing object pixels without renderer restart");
        Check(expectedColor > 100, "surface factors change while authored alpha coverage remains intact");
        Check(!renderer.ReloadMaterials(std::span(&invalid, 1)).success, "renderer rejects invalid surface edit");
        settings.reset = true; draw(); const auto retained = ReadBase(device, resources);
        Check(std::memcmp(after.data(), retained.data(), after.size() * sizeof(render::Float4)) == 0,
            "failed compile retains identical visible material evaluation");
        std::cout << "Material script atomic reload tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
