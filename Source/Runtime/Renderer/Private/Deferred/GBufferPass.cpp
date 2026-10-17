// 算法通过管线契约和资源集合接入帧，不依赖帧调度实现。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Pipeline/PipelineCache.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"

namespace render {
void AddGBufferPass(RenderGraph& graph, PassContext& context) {
    using namespace dx12;
    auto& targets = context.targets;
    auto parameters = context.Parameters(graph, "Raster.GBuffer", context.ViewParameters(), {&context.scene.Triangles(), &context.scene.Materials(),
        &context.scene.TriangleAttributes(), &context.scene.TextureViews(), &context.scene.Texels()});
    const std::array<GpuResource*, 5> images{targets.gbuffer.position.Pointer(), targets.gbuffer.normal.Pointer(),
        targets.gbuffer.base.Pointer(), targets.gbuffer.tangent.Pointer(), targets.gbuffer.uv.Pointer()};
    // 附件的清除写入和着色器输入共同参与编译，不隐式依赖一张全局目标列表。
    for (auto* image : images) parameters.Access(*image, ResourceAccess::Write, D3D12_RESOURCE_STATE_RENDER_TARGET);
    parameters.Access(targets.gbuffer.depth.Resource(), ResourceAccess::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    parameters.AddPass(graph, "Raster.GBuffer", [&context, &graph, images](const PassParameters& bound) {
        // 原生 RTV/DSV 描述符绑定当前图的分配，物理资源可能在帧间复用。
        const auto& targets = context.targets;
        for (unsigned index = 0; index < images.size(); ++index)
            context.device.Native()->CreateRenderTargetView(images[index]->object.Get(), nullptr, context.descriptors.Rtv(2 + index));
        context.device.Native()->CreateDepthStencilView(targets.gbuffer.depth.Resource().object.Get(), nullptr, context.descriptors.Dsv());
        auto* commands = context.device.Commands();
        const float zero[4]{};
        D3D12_CPU_DESCRIPTOR_HANDLE views[]{context.descriptors.Rtv(2), context.descriptors.Rtv(3), context.descriptors.Rtv(4),
            context.descriptors.Rtv(5), context.descriptors.Rtv(6)};
        for (const auto view : views) commands->ClearRenderTargetView(view, zero, 0, nullptr);
        const auto depth = context.descriptors.Dsv();
        commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        commands->OMSetRenderTargets(5, views, FALSE, &depth);
        D3D12_VIEWPORT viewport{0, 0, static_cast<float>(context.targets.width), static_cast<float>(context.targets.height), 0, 1};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(context.targets.width), static_cast<LONG>(context.targets.height)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        context.Bind(graph, bound, true);
        commands->SetPipelineState(context.pipelines.GBuffer());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // 着色器顶点拉取严格共用两种光线查询后端的三角形顺序。
        commands->DrawInstanced(context.scene.TriangleCount() * 3, 1, 0, 0);
    });
}
}
