// 屏幕空间 GI 通过管线契约访问当前帧与视图历史资源。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "render/RenderSettings.h"

namespace render {
void AddSSGIPass(RenderGraph& graph, PassContext& context) {
    auto& targets = context.targets;
    // 屏幕投影行进本身不查询场景加速结构，历史输入由独立视图历史提供。
    auto inputs = context.ReceiverReads();
    const auto emitters = context.LightingReads(false);
    inputs.insert(inputs.end(), emitters.begin(), emitters.end());
    inputs.insert(inputs.end(), {targets.lighting.direct.Pointer(), targets.history.gi, targets.history.position, targets.history.normal});
    // 屏幕行进、历史拒绝与天空边界只捕获本算法真正消费的设置。
    auto parameters = context.ViewParameters();
    const auto& settings = context.settings;
    parameters.sampling = {static_cast<float>(settings.samples), 64, 0.1f, 0};
    parameters.flags.x = settings.reset ? 1u : 0u;
    parameters.flags.y = settings.temporalReuse ? 1u : 0u;
    parameters.restir.z = 0.9f;
    parameters.restir.w = 0.035f;
    parameters.post.x = 0.92f;
    parameters.screenFlags.z = settings.ssil ? 1u : 0u;
    parameters.screenStrength.z = settings.ssilIntensity;
    parameters.sky = {settings.sky ? 1.0f : 0.0f, settings.skyIntensity, 0, 0};
    parameters.lighting.x = context.scene.LightCount();
    context.Compute(graph, "GI.SSGI", L"SSGI", parameters, std::move(inputs), targets.lighting.gi.Resource());
}
}
