#include "DeferredRenderer.h"
// 延迟管线组合各算法入口，共享资源从明确的资源层取得。
#include "Resources/FrameResources.h"
#include "Pipeline/PassContext.h"
#include "Pipeline/RenderPasses.h"
#include "Scene/SceneGpu.h"
#include "Shadows/ShadowRenderer.h"
#include "render/RenderPipeline.h"
#include "render/RenderSettings.h"

namespace render {
void DeferredRender(PassContext& pass, ShadowRenderer& shadows, RenderPipeline& pipeline, RenderPipelineContext& frame,
    const std::function<void(RenderGraph&)>& finalOverlay) {
    auto& graph = frame.graph;
    auto& resources = pass.targets;
    // 黑板只发布当前图注册的逻辑资源，扩展不需要依赖 Renderer 的私有帧类型。
    frame.Publish("Scene.Position", resources.gbuffer.position.Resource());
    frame.Publish("Scene.Normal", resources.gbuffer.normal.Resource());
    frame.Publish("Scene.BaseColor", resources.gbuffer.base.Resource());
    frame.Publish("Scene.Depth", resources.gbuffer.depth.Resource());
    frame.Publish("Lighting.Direct", resources.lighting.direct.Resource());
    frame.Publish("Lighting.GI", resources.lighting.gi.Resource());
    frame.Publish("Post.Display", *resources.display);

    pipeline.ExecuteStage(RenderStage::Prepare, frame, [&](auto&) {
        pass.scene.AddUploadPasses(graph, pass.executor, pass.backend);
    });
    pipeline.ExecuteStage(RenderStage::Shadows, frame, [&](auto&) {
        if (pass.settings.shadows && pass.settings.shadowMode != 0)
            shadows.AddPasses(graph, pass, pass.scene.ShadowViewCount(), pass.settings.shadowMode);
    });
    pipeline.ExecuteStage(RenderStage::BasePass, frame, [&](auto&) { AddGBufferPass(graph, pass); });
    pipeline.ExecuteStage(RenderStage::DirectLighting, frame, [&](auto&) {
        if (pass.giMode != 0) AddDirectLightingPass(graph, pass);
    });
    pipeline.ExecuteStage(RenderStage::GlobalIllumination, frame, [&](auto&) {
        if (pass.giMode == 0) AddPathTracingPass(graph, pass);
        else if (pass.giMode == 1) AddSSGIPass(graph, pass);
        else AddReSTIRGIPasses(graph, pass);
    });
    pipeline.ExecuteStage(RenderStage::Denoise, frame, [&](auto&) {
        AddRadianceResolvePasses(graph, pass);
        frame.Publish("Scene.Color", resources.denoiser.filtered.Resource());
    });
    pipeline.ExecuteStage(RenderStage::ScreenEffects, frame, [&](auto&) {
        // 上游扩展可发布替代 HDR，下一固定阶段直接消费它，而不是绕回内置全局目标。
        resources.presentation = &frame.Require("Scene.Color");
        AddScreenSpaceEffectsPasses(graph, pass, pass.settings);
        frame.Publish("Scene.Color", *resources.presentation);
    });
    pipeline.ExecuteStage(RenderStage::PostProcess, frame, [&](auto&) {
        resources.presentation = &frame.Require("Scene.Color");
        AddPostProcessPass(graph, pass);
    });
    pipeline.ExecuteStage(RenderStage::History, frame, [&](auto&) {
        AddFrameHistoryPasses(graph, pass, pass.executor, pass.giMode);
        resources.ExtractDiagnostics(graph, pass.executor, pass.giMode);
    });
    // 主场景所有阶段建图完毕后才进入编辑器覆盖层，后续统一编译执行。
    pipeline.ExecuteStage(RenderStage::Overlay, frame, [&](auto&) {
        if (finalOverlay) finalOverlay(graph);
    });
}
}
