#pragma once
// 跨帧存储随视图维护，帧资源仅在提取边界提供当前结果。
#include "dx12/Dx12Common.h"

namespace render::dx12 { class ResourcePool; class GraphExecutor; }
namespace render {
class FrameResources;

// 视图历史跨图保存，负责长期资源分配与本帧结果的显式提取。
class ViewHistory {
public:
    ViewHistory(dx12::ResourcePool&, unsigned width, unsigned height);
    void QueueExtraction(RenderGraph&, dx12::GraphExecutor&, FrameResources&, unsigned giMode);
    dx12::GpuResource* gi{}; // 上一帧间接光或路径追踪均值。
    dx12::GpuResource* position{}; // 上一帧可见表面位置，用于重投影拒绝。
    dx12::GpuResource* normal{}; // 上一帧可见表面法线，用于历史兼容性检查。
    dx12::GpuResource* reservoir{}; // 上一帧最终 ReSTIR 结构缓冲区。
    dx12::GpuResource* resolved{}; // 上一帧时间重建辐射及置信度。
};
}
