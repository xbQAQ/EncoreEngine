// 路径追踪只借用帧资源，资源寿命由帧和视图所有者维护。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "render/RenderSettings.h"

namespace render {
void AddPathTracingPass(RenderGraph& graph, PassContext& context) {
    // 相机路径从场景直接求交，不读取当前 GBuffer；光照显式补充所选阴影资源。
    auto inputs = context.RaySceneReads();
    const auto lighting = context.LightingReads();
    inputs.insert(inputs.end(), lighting.begin(), lighting.end());
    inputs.push_back(context.targets.history.gi);
    // 累积保持在线性辐射域，显示滤波不能反馈到参考路径积分。
    auto parameters = context.ViewParameters();
    const auto& settings = context.settings;
    parameters.settings.w = static_cast<unsigned>(settings.pathVertices);
    parameters.sampling.x = static_cast<float>(settings.samples);
    parameters.flags.x = settings.reset ? 1u : 0u;
    parameters.sky = {settings.sky ? 1.0f : 0.0f, settings.skyIntensity, 0, 0};
    // NEE 与路径可见性使用本 Pass 自己捕获的光源和阴影契约。
    parameters.lighting = {context.scene.LightCount(), context.scene.ShadowViewCount(), settings.shadowMode, 10};
    parameters.shadow = {256, settings.shadowBias, settings.shadowNormalBias, settings.shadowRadius};
    parameters.variance = {settings.vsmMinVariance, settings.vsmBleedReduction, 0, 0};
    parameters.flags.w = settings.shadows ? 1u : 0u;
    context.Compute(graph, "GI.PathTracing", L"PathTrace", parameters, std::move(inputs), context.targets.lighting.gi.Resource());
}
}
