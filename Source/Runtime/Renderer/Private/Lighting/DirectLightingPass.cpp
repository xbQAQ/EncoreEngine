// 光照算法通过管线上下文消费当前帧资源。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "Lighting/LtcLuts.h"
#include "Scene/SceneGpu.h"
#include "render/RenderSettings.h"

namespace render {
void AddDirectLightingPass(RenderGraph& graph, PassContext& context) {
    // 接收点与光照分别声明，只有射线阴影选择场景求交资源。
    auto inputs = context.ReceiverReads();
    const auto lighting = context.LightingReads();
    inputs.insert(inputs.end(), lighting.begin(), lighting.end());
    // GGX LTC 数据属于本 Pass 的着色参数，不从场景或常量缓冲区隐式取得。
    if (context.backend == 2) {
        if (!context.ltc) throw std::logic_error("Raster direct lighting requires LTC tables");
        inputs.insert(inputs.end(), {&context.ltc->Matrix(), &context.ltc->Amplitude()});
    }
    // 混合积分器组合此直接光项，参考相机路径已自行包含直接光。
    auto parameters = context.ViewParameters();
    const auto& settings = context.settings;
    parameters.lighting = {context.scene.LightCount(), context.scene.ShadowViewCount(), settings.shadowMode, 10};
    parameters.shadow = {256, settings.shadowBias, settings.shadowNormalBias, settings.shadowRadius};
    parameters.variance = {settings.vsmMinVariance, settings.vsmBleedReduction, 0, 0};
    parameters.flags.w = settings.shadows ? 1u : 0u;
    parameters.sampling.x = static_cast<float>(settings.samples);
    context.Compute(graph, "Lighting.Direct", L"DirectLighting", parameters, std::move(inputs), context.targets.lighting.direct.Resource());
}
}
