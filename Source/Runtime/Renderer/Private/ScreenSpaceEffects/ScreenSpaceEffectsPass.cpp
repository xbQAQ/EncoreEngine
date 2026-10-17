// 屏幕效果只消费管线提供的输入与资源，不接管帧调度。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "render/RenderSettings.h"

namespace render {
void AddScreenSpaceEffectsPasses(RenderGraph& graph, PassContext& context, const RenderSettings& settings) {
    auto& targets = context.targets;
    // 上游固定阶段或扩展已经选择 HDR 输入，这里沿启用的效果继续传递。
    // 每一步借用上一份不可变 HDR，图据此推导寿命并复用不重叠的分配。
    const auto compose = [&](const char* label, const wchar_t* shader, dx12::GpuResource& output,
        std::vector<dx12::GpuResource*> additional, FrameConstants parameters) {
        auto* input = targets.presentation;
        additional.push_back(input);
        // 参数由各效果入口按值交入，此合成步骤只组织输入输出和 RDG 分派。
        context.Compute(graph, label, shader, parameters, std::move(additional), output, false, nullptr, input);
        targets.presentation = &output;
    };
    // 天空先于屏幕反射进入输入，使可见天空也能参与 SSR 的颜色查询。
    if (settings.sky) {
        auto parameters = context.ViewParameters();
        parameters.sky = {1, settings.skyIntensity, 0, 0};
        compose("Sky.Background", L"Sky", targets.effects.sky.Resource(), {targets.gbuffer.position.Pointer()}, parameters);
    }
    // AO 的半径和强度只进入遮蔽 Pass，不携带反射或扩散参数。
    if (settings.ssao) {
        auto parameters = context.ViewParameters();
        parameters.screenStrength.x = settings.ssaoIntensity;
        parameters.screenGeometry.x = settings.ssaoRadius;
        compose("Effects.SSAO", L"SSAO", targets.effects.ambientOcclusion.Resource(), context.ReceiverReads(), parameters);
    }
    // 反射独立拥有行进预算、厚度与最大距离，不借用其他屏幕算法的快照。
    if (settings.ssr) {
        auto parameters = context.ViewParameters();
        parameters.screenStrength.y = settings.ssrIntensity;
        parameters.screenGeometry.y = settings.ssrDistance;
        parameters.sampling.y = 64;
        parameters.sampling.z = 0.1f;
        compose("Effects.SSR", L"SSR", targets.effects.reflections.Resource(), context.ReceiverReads(), parameters);
    }
    // SSIL 连接可见接收点，声明发光体身份和已积分辐射，不引入场景求交。
    if (settings.ssil) {
        auto reads = context.ReceiverReads();
        const auto emitters = context.LightingReads(false);
        reads.insert(reads.end(), emitters.begin(), emitters.end());
        if (context.giMode != 0) reads.push_back(targets.lighting.gi.Pointer());
        if (context.giMode == 1) reads.push_back(targets.lighting.direct.Pointer());
        auto* input = targets.presentation;
        reads.push_back(input);
        // SSIL 根据解析光源身份扣除已被 NEE 覆盖的发射，必须单独填写光源数量。
        auto parameters = context.ViewParameters();
        parameters.screenStrength.z = settings.ssilIntensity;
        parameters.sampling = {static_cast<float>(settings.samples), 64, 0.1f, 0};
        parameters.controls.z = settings.giIntensity;
        parameters.lighting.x = context.scene.LightCount();
        context.Compute(graph, "Effects.SSIL", L"SSIL", parameters, std::move(reads), targets.effects.indirectLight.Resource(), false, nullptr, input);
        targets.presentation = targets.effects.indirectLight.Pointer();
    }
    // 双向扩散只跨越同材质且位置连续的表面，避免颜色穿过几何轮廓。
    if (settings.sss) {
        auto horizontalParameters = context.ViewParameters();
        horizontalParameters.screenStrength.w = settings.sssIntensity;
        horizontalParameters.screenGeometry.z = settings.sssRadius;
        compose("Effects.SSS.Horizontal", L"SSS_Horizontal", targets.effects.subsurfaceHorizontal.Resource(), context.ReceiverReads(), horizontalParameters);
        auto verticalParameters = context.ViewParameters();
        verticalParameters.screenStrength.w = settings.sssIntensity;
        verticalParameters.screenGeometry.z = settings.sssRadius;
        compose("Effects.SSS.Vertical", L"SSS_Vertical", targets.effects.subsurface.Resource(), context.ReceiverReads(), verticalParameters);
    }
}
}
