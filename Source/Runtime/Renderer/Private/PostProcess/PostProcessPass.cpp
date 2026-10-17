// 后处理只接收管线输出和帧资源，不依赖编辑器宿主。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "render/RenderSettings.h"

namespace render {
void AddPostProcessPass(RenderGraph& graph, PassContext& context) {
    // 曝光、泛光和介质参数在显示 Pass 入口捕获，不写入视图历史或其他算法快照。
    const auto& settings = context.settings;
    auto parameters = context.ViewParameters();
    parameters.controls.x = settings.exposure;
    parameters.controls.y = settings.bloom;
    parameters.post = {0, 1, settings.mediumDensity, settings.phaseG};
    parameters.timing.x = context.delta;
    parameters.timing.y = settings.autoExposure ? 1.0f : 0.0f;
    // 色调映射只读取 HDR；体积介质启用后才声明距离图像、光源和所选可见性资源。
    std::vector<dx12::GpuResource*> inputs{context.targets.presentation};
    if (context.settings.mediumDensity > 0) {
        inputs.push_back(context.targets.gbuffer.position.Pointer());
        const auto lighting = context.LightingReads();
        inputs.insert(inputs.end(), lighting.begin(), lighting.end());
        parameters.lighting = {context.scene.LightCount(), context.scene.ShadowViewCount(), settings.shadowMode, 10};
        parameters.shadow = {256, settings.shadowBias, settings.shadowNormalBias, settings.shadowRadius};
        parameters.variance = {settings.vsmMinVariance, settings.vsmBleedReduction, 0, 0};
        parameters.flags.w = settings.shadows ? 1u : 0u;
    }
    // 在编辑器绘制前完成效果链的色调映射和显示编码。
    context.Compute(graph, "Post.VolumeBloomACES", L"Post", parameters, std::move(inputs), *context.targets.display,
        false, nullptr, context.targets.presentation);
}
}
