// 复用算法通过资源集合访问 reservoir，跨帧所有权留在视图层。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "render/RenderSettings.h"

namespace render {
namespace {
FrameConstants TracingParameters(const PassContext& context) {
    // 候选与最终求值都包含本算法的次级路径追踪，分别取得独立的 CPU 参数快照。
    auto parameters = context.ViewParameters();
    const auto& settings = context.settings;
    parameters.settings.w = static_cast<unsigned>(settings.pathVertices);
    parameters.sampling.x = static_cast<float>(settings.samples);
    parameters.sky = {settings.sky ? 1.0f : 0.0f, settings.skyIntensity, 0, 0};
    parameters.lighting = {context.scene.LightCount(), context.scene.ShadowViewCount(), settings.shadowMode, 10};
    parameters.shadow = {256, settings.shadowBias, settings.shadowNormalBias, settings.shadowRadius};
    parameters.variance = {settings.vsmMinVariance, settings.vsmBleedReduction, 0, 0};
    parameters.flags.w = settings.shadows ? 1u : 0u;
    return parameters;
}
}

void AddReSTIRGIPasses(RenderGraph& graph, PassContext& context) {
    auto& targets = context.targets;
    const auto& settings = context.settings;
    // 候选生成与最终求值会追踪次级路径，复用阶段仅从 GBuffer 恢复接收点。
    auto tracedReads = context.ReceiverReads();
    const auto queries = context.RaySceneReads();
    tracedReads.insert(tracedReads.end(), queries.begin(), queries.end());
    const auto lighting = context.LightingReads();
    tracedReads.insert(tracedReads.end(), lighting.begin(), lighting.end());
    // 新候选只设置每采样提议数和有效 M 上限，不携带历史复用或显示参数。
    auto candidateParameters = TracingParameters(context);
    candidateParameters.counts.w = static_cast<unsigned>(settings.candidates);
    candidateParameters.restir.x = static_cast<float>(settings.maxHistory);
    context.Compute(graph, "ReSTIR.Candidates", L"ReSTIR_Candidate", candidateParameters, tracedReads, targets.restir.candidates.Resource(), true);
    // 时间复用只设置 reservoir 与历史控制，不继承候选追踪阶段的常量快照。
    auto temporalParameters = context.ViewParameters();
    temporalParameters.flags.x = settings.reset ? 1u : 0u;
    temporalParameters.flags.y = settings.temporalReuse ? 1u : 0u;
    temporalParameters.restir = {static_cast<float>(settings.maxHistory), 0, 0.9f, 0.035f};
    auto temporalReads = context.ReceiverReads();
    temporalReads.insert(temporalReads.end(), {targets.restir.candidates.Pointer(), targets.history.reservoir, targets.history.position,
        targets.history.normal});
    context.Compute(graph, "ReSTIR.Temporal", L"ReSTIR_Temporal", temporalParameters, std::move(temporalReads), targets.restir.temporal.Resource(), true);
    // 空间复用从自己的视图快照开始，仅消费不可变时间结果和邻域设置。
    auto spatialParameters = context.ViewParameters();
    spatialParameters.flags.z = settings.spatialReuse ? 1u : 0u;
    spatialParameters.restir = {static_cast<float>(settings.maxHistory), static_cast<float>(settings.spatialRadius), 0.9f, 0.035f};
    auto spatialReads = context.ReceiverReads();
    spatialReads.push_back(targets.restir.temporal.Pointer());
    context.Compute(graph, "ReSTIR.Spatial", L"ReSTIR_Spatial", spatialParameters, std::move(spatialReads), targets.restir.spatial.Resource(), true);
    auto evaluateReads = std::move(tracedReads);
    evaluateReads.push_back(targets.restir.spatial.Pointer());
    // 求值将 t11 指向最终不可变空间结果，并检查重新连接线段的可见性。
    auto evaluateParameters = TracingParameters(context);
    evaluateParameters.restir.x = static_cast<float>(settings.maxHistory);
    evaluateParameters.sampling.w = static_cast<float>(settings.debugView);
    context.Compute(graph, "ReSTIR.Evaluate", L"ReSTIR_Evaluate", evaluateParameters, std::move(evaluateReads), targets.lighting.gi.Resource(), false,
        targets.restir.spatial.Pointer());
}
}
