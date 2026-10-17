// 视图历史借用当前帧资源执行提取，不拥有帧调度对象。
#include "View/ViewHistory.h"
#include "Resources/FrameResources.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"

namespace render {
ViewHistory::ViewHistory(dx12::ResourcePool& pool, unsigned width, unsigned height) {
    // 历史是跨图外部存储，不与本帧瞬时资源共用逻辑寿命。
    constexpr auto writable = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto texture = [&](const char* name, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        return &pool.Texture(name, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, flags);
    };
    gi = texture("History.GI");
    // 零拷贝提取交换完整物理纹理，历史能力必须与对应 GBuffer 源精确兼容。
    position = texture("History.Position", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | writable);
    normal = texture("History.Normal", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | writable);
    reservoir = &pool.Buffer("History.Reservoir", static_cast<UINT64>(width) * height * 96, 96, writable);
    resolved = texture("History.Resolved");
}

void ViewHistory::QueueExtraction(RenderGraph& graph, dx12::GraphExecutor& executor, FrameResources& frame, unsigned giMode) {
    // 提取边界由历史所有者统一维护，各算法只负责产生本帧图内结果。
    executor.QueueExtraction(graph, "History.GI", frame.lighting.gi.Resource(), *gi);
    executor.QueueExtraction(graph, "History.Position", frame.gbuffer.position.Resource(), *position);
    executor.QueueExtraction(graph, "History.Normal", frame.gbuffer.normal.Resource(), *normal);
    executor.QueueExtraction(graph, "History.Resolved", frame.denoiser.resolved.Resource(), *resolved);
    if (giMode == 2) executor.QueueExtraction(graph, "History.Reservoir", frame.restir.spatial.Resource(), *reservoir);
}
}
