#pragma once
// Pass 上下文是管线到资源和场景的借用边界，不拥有帧生命周期。
#include "dx12/Dx12Common.h"
#include "dx12/PassParameters.h"
#include "render/GpuTypes.h"
#include <array>
#include <vector>

namespace render::dx12 { class Dx12Device; class DescriptorArena; class GraphExecutor; }
namespace render {
class PipelineCache;
class SceneGpu;
class FrameResources;
class ViewState;
class LtcLuts;
struct RenderSettings;

// 帧局部录制上下文向 Pass 暴露资源，但不拥有其寿命。
class PassContext {
public:
    PassContext(dx12::Dx12Device&, dx12::DescriptorArena&, dx12::GraphExecutor&, PipelineCache&, SceneGpu&, FrameResources&,
        const ViewState&, const RenderSettings&, float delta);
    FrameConstants ViewParameters() const;
    dx12::PassParameters Parameters(RenderGraph&, const char* label, FrameConstants values, const std::vector<dx12::GpuResource*>& reads,
        dx12::GpuResource* output = nullptr, bool reservoirOutput = false,
        dx12::GpuResource* selectedReservoir = nullptr, dx12::GpuResource* postInput = nullptr,
        dx12::GpuResource* shadowInput = nullptr) const;
    void Bind(const RenderGraph&, const dx12::PassParameters&, bool graphics = false);
    void Compute(RenderGraph&, const char* label, const std::wstring& shader, FrameConstants values, std::vector<dx12::GpuResource*> reads,
        dx12::GpuResource& output, bool reservoirOutput = false,
        dx12::GpuResource* selectedReservoir = nullptr, dx12::GpuResource* postInput = nullptr);
    // 材质求值只读取材质表与纹理资产，不隐含几何、光源或加速结构。
    std::vector<dx12::GpuResource*> MaterialReads() const;
    // 可见接收点从 GBuffer 恢复材质和真实几何法线，供屏幕算法显式组合。
    std::vector<dx12::GpuResource*> ReceiverReads() const;
    // 只有调用场景求交的算法声明当前后端的 BVH 或 TLAS。
    std::vector<dx12::GpuResource*> RaySceneReads() const;
    // 仅发射体身份查询可关闭可见性依赖；完整光照按当前阴影方式补充输入。
    std::vector<dx12::GpuResource*> LightingReads(bool visibility = true) const;
    dx12::Dx12Device& device; // 原生命令录制目标。
    dx12::DescriptorArena& descriptors; // 逐 Pass 表分配及长期视图访问。
    dx12::GraphExecutor& executor; // 将各 Pass 的 CPU 参数快照声明为图拥有的延迟上传。
    PipelineCache& pipelines; // 共用的不可变管线。
    SceneGpu& scene; // 当前几何和加速结构资源。
    FrameResources& targets; // 按算法用途分组的帧资源与独立历史边界。
    const ViewState& view; // 只包含 CPU 相机历史与采样序号的视图状态。
    const RenderSettings& settings; // 当前固定管线的只读设置，各 Pass 按职责选择参数。
    float delta; // 当前帧间隔，仅由需要时间参数的算法使用。
    unsigned backend; // 编辑器选择的求交变体。
    unsigned giMode; // 当前有效积分器，仅声明着色器实际可见且已初始化的输入。
    dx12::GpuResource* shadowAtlas{}; // 光照 Pass 之前生产的已选原始或滤波图集。
    LtcLuts* ltc{}; // 可选的独立 LTC 数据所有者，仅光栅面光源算法显式读取。
};
}
