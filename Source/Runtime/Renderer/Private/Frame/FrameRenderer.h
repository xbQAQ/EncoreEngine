#pragma once
// 帧调度层显式组合视图、资源和管线，算法实现仍留在各自目录。
#include "View/ViewState.h"
#include "Pipeline/PipelineCache.h"
#include "Resources/FrameResources.h"
#include "Scene/SceneGpu.h"
#include "Lighting/LtcLuts.h"
#include "Shadows/ShadowRenderer.h"
#include "dx12/GraphProfiler.h"
#include "render/MaterialScripts.h"
#include "render/RenderPipeline.h"
#include <functional>
#include <optional>

namespace render {
// 组合算法 Pass；编辑器代码仅提供最终覆盖层的注册回调。
class FrameRenderer {
public:
    FrameRenderer(dx12::Dx12Device&, dx12::DescriptorArena&, dx12::ResourcePool&, unsigned width, unsigned height,
        Scene scene = BuildCornellScene());
    const Scene& ReferenceScene() const { return scene_.ReferenceScene(); }
    void Draw(const gameplay::RenderSnapshot&, RenderSettings&, float delta, dx12::GpuResource& backbuffer,
        const std::function<void(RenderGraph&)>& finalOverlay);
    dx12::GpuResource& Display() const { return *frame_.display; }
    unsigned Accumulated() const { return view_.Accumulated(); }
    RenderPipeline& Pipeline() { return *pipeline_; }
    void UsePipeline(RenderPipeline& pipeline) { pipeline_ = &pipeline; }
    const std::vector<std::string>& PassNames() const { return passNames_; }
    const GraphProfile& Profile() const { return profiler_.Latest(); }
    // 默认关闭采集；停止后保留最后完成的样本供宿主接收和回看。
    void SetProfilingEnabled(bool enabled) { profilingEnabled_ = enabled; }
    void RetainFrameImages() { frame_.RetainFrameImages(); }
    MaterialCompileResult ReloadMaterials(std::span<const MaterialShaderSource> sources);
    void RequestMaterialReload(std::span<const MaterialShaderSource> sources);
    std::optional<MaterialCompileResult> PollMaterialReload();
    bool MaterialCompilationPending() const { return pipelines_.MaterialCompilationPending(); }
private:
    dx12::Dx12Device& device_; // 队列所有者，寿命长于全部帧对象。
    dx12::DescriptorArena& descriptors_; // 视图表分配器，每个已完成帧之后重置。
    dx12::ResourcePool& resources_; // 与捕获服务共用的 GPU 分配所有者。
    SceneGpu scene_; // 快照提取、上传和加速结构。
    LtcLuts ltc_; // 与场景和全局参数解耦的只读面光源拟合表。
    FrameResources frame_; // 按可见表面、光照、复用、重建和历史职责划分的帧资源。
    ShadowRenderer shadows_; // 共用的光源视图图集与 VSM 滤波 Pass。
    PipelineCache pipelines_; // 不可变根签名及算法程序。
    ViewState view_; // 只保留 CPU 相机与采样历史，不持有算法 GPU 常量。
    RenderPipeline builtinPipeline_; // 未注入宿主扩展注册表时使用的默认固定阶段管线。
    RenderPipeline* pipeline_{&builtinPipeline_}; // 可跨场景复用的宿主扩展注册表。
    dx12::GraphProfiler profiler_; // 可复用 CPU/GPU 计时存储，仅在对应提交 fence 完成后读取。
    bool profilingEnabled_{}; // 只有显式录制会话才将原生采集器接入执行器。
    std::vector<std::string> passNames_; // 供 RDG 检查器查看的最近编译帧。
};
}
