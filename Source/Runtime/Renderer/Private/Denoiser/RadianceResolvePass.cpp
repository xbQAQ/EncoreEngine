// 重建算法借用当前帧与历史资源，统一通过管线入口注册。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "render/RenderSettings.h"

namespace render {
void AddRadianceResolvePasses(RenderGraph& graph, PassContext& context) {
    auto& targets = context.targets;
    // 重建只消费已积分辐射与可见接收点，不声明场景求交、光源或阴影资源。
    auto temporalReads = context.ReceiverReads();
    temporalReads.insert(temporalReads.end(), {targets.lighting.gi.Pointer(), targets.history.position, targets.history.normal, targets.history.resolved});
    if (context.giMode != 0) temporalReads.push_back(targets.lighting.direct.Pointer());
    // 时间重建独立拥有混合增益、采样置信度和历史拒绝，不继承积分器的参数对象。
    auto temporalParameters = context.ViewParameters();
    const auto& settings = context.settings;
    temporalParameters.controls.z = settings.giIntensity;
    temporalParameters.sampling.x = static_cast<float>(settings.samples);
    temporalParameters.sampling.w = static_cast<float>(settings.debugView);
    temporalParameters.flags.x = settings.reset ? 1u : 0u;
    temporalParameters.flags.y = settings.temporalReuse ? 1u : 0u;
    temporalParameters.restir.z = 0.9f;
    temporalParameters.restir.w = 0.035f;
    temporalParameters.post.x = 0.92f;
    context.Compute(graph, "Resolve.Temporal", L"RadianceResolve_Temporal", temporalParameters, std::move(temporalReads),
        targets.denoiser.resolved.Resource(), false, nullptr, targets.history.resolved);
    // 空间重建使用独立输入图像，避免原地邻域读写竞争。
    auto spatialParameters = context.ViewParameters();
    spatialParameters.sampling.w = static_cast<float>(settings.debugView);
    spatialParameters.restir.z = 0.9f;
    auto spatialReads = context.ReceiverReads();
    spatialReads.push_back(targets.denoiser.resolved.Pointer());
    context.Compute(graph, "Resolve.Spatial", L"RadianceResolve_Spatial", spatialParameters, std::move(spatialReads), targets.denoiser.filtered.Resource(), false, nullptr,
        targets.denoiser.resolved.Pointer());
}
}
