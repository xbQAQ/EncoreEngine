// 帧入口通过管线上下文建图，不接管各算法的实现归属。
#include "Frame/FrameRenderer.h"
#include "Pipeline/PassContext.h"
#include "Pipeline/RenderPasses.h"
#include "Deferred/DeferredRenderer.h"
#include "render/RenderSettings.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"

namespace render {
FrameRenderer::FrameRenderer(dx12::Dx12Device& device, dx12::DescriptorArena& descriptors,
    dx12::ResourcePool& resources, unsigned width, unsigned height, Scene source)
    : device_(device), descriptors_(descriptors), resources_(resources), scene_(device, descriptors, resources, std::move(source)),
      ltc_(resources), frame_(descriptors, resources, width, height), shadows_(device, descriptors, resources), pipelines_(device),
      profiler_(device) {
    // 初始化使用独立图，避免其导出与逐帧历史导出冲突。
    device_.BeginCommands();
    descriptors_.Bind(device_.Commands());
    RenderGraph graph;
    dx12::GraphExecutor executor(device_);
    executor.Import(graph, resources_);
    ltc_.Upload(graph, executor, device_);
    resources_.ClearWritable(graph);
    executor.Execute(graph);
    device_.SubmitAndWait();
}

MaterialCompileResult FrameRenderer::ReloadMaterials(std::span<const MaterialShaderSource> sources) {
    // 替换管线之前验证全部变体，成功后才发布材质程序。
    return pipelines_.ReloadMaterials(sources);
}

void FrameRenderer::RequestMaterialReload(std::span<const MaterialShaderSource> sources) {
    // 后台编译捕获源代码值；原生管线发布仍在渲染线程进行。
    pipelines_.RequestMaterialReload(sources);
}

std::optional<MaterialCompileResult> FrameRenderer::PollMaterialReload() {
    // 轮询仅发布完整候选集合，并在不等待 DXC 的情况下返回诊断。
    return pipelines_.PollMaterialReload();
}

void FrameRenderer::Draw(const gameplay::RenderSnapshot& snapshot, RenderSettings& settings, float delta,
    dx12::GpuResource& backbuffer, const std::function<void(RenderGraph&)>& finalOverlay) {
    // 有效设置选择专用光栅着色器变体，同时保留用户的光线模式配置。
    RenderSettings effective = settings;
    if (effective.pipeline == 0) {
        effective.backend = 2;
        effective.gi = 1;
        if (effective.shadowMode == 0) effective.shadowMode = 2;
    }
    if (effective.backend == 1 && !device_.DxrSupported()) throw std::runtime_error("DXR 1.1 is unavailable");
    if (scene_.Synchronize(snapshot)) settings.reset = true;
    effective.reset = settings.reset;
    scene_.PrepareBackend(effective.backend);
    // 诊断快照属于可选外部存储，必须在导入本帧资源之前准备。
    frame_.PrepareDiagnostics(resources_);
    view_.Begin(snapshot, frame_.width, frame_.height, effective.reset);
    device_.BeginCommands();
    descriptors_.ResetFrame();
    descriptors_.Bind(device_.Commands());
    RenderGraph graph;
    // 空闲帧完全绕过逐 Pass 计时和 GPU 查询，而非仅冻结性能面板。
    dx12::GraphExecutor executor(device_, profilingEnabled_ ? &profiler_ : nullptr);
    executor.Import(graph, resources_);
    executor.Import(graph, backbuffer);
    frame_.DeclareFrame(graph, executor);
    PassContext context(device_, descriptors_, executor, pipelines_, scene_, frame_, view_, effective, delta);
    // 只有选择 LTC 的直接光 Pass 显式绑定两张表，其他算法不产生隐式依赖。
    context.ltc = &ltc_;
    context.shadowAtlas = &shadows_.Atlas(effective.shadowMode);
    // 固定入口只搭建阶段，各算法和外部模块通过参数向同一 RDG 添加工作。
    RenderPipelineContext pipelineFrame(graph, frame_.width, frame_.height, effective);
    pipelineFrame.AttachRhi(executor, device_, descriptors_);
    DeferredRender(context, shadows_, *pipeline_, pipelineFrame, finalOverlay);
    graph.Export(backbuffer.graphId, D3D12_RESOURCE_STATE_PRESENT);
    graph.Export(frame_.display->graphId, dx12::ShaderRead);
    executor.Execute(graph);
    passNames_ = executor.PassNames();
    device_.SubmitAndWait();
    if (profilingEnabled_) profiler_.CollectCompleted();
    view_.Complete(settings);
}
}
