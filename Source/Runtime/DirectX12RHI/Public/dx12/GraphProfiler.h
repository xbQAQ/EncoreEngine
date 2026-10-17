#pragma once

#include "dx12/Dx12Common.h"
#include "render/GraphProfile.h"
#include <chrono>

namespace render::dx12 {
class Dx12Device;

// 在同步且栅栏已完成的帧之间复用时间戳分配。
class GraphProfiler {
public:
    explicit GraphProfiler(Dx12Device& device);
    void BeginFrame(size_t passCount, double cpuCompileMilliseconds = 0);
    void BeginPass(const CompiledPass& pass);
    void EndPass();
    void RecordBarrier(double cpuMilliseconds);
    void EndFrame();
    // 不阻塞或提前映射，仅在发布新完成样本时返回真。
    bool CollectCompleted();
    const GraphProfile& Latest() const { return latest_; }

private:
    using Clock = std::chrono::steady_clock;
    void EnsureCapacity(unsigned queryCount);

    Dx12Device& device_; // 测量通道所在命令列表的设备与直接队列。
    ComPtr<ID3D12QueryHeap> queries_; // 对应提交完成前保留的时间戳槽。
    ComPtr<ID3D12Resource> readback_; // 由 ResolveQueryData 填充的 COPY_DEST 回读缓冲。
    GraphProfile latest_; // 当前 CPU 样本及栅栏完成后的对应 GPU 时间。
    Clock::time_point frameStart_; // 编译完成后图录制的 CPU 起点。
    Clock::time_point passStart_; // 活动通道前置屏障之前的 CPU 起点。
    UINT64 timestampFrequency_{}; // 独立于 CPU 时钟的真实直接队列频率。
    UINT64 priorSubmissionFence_{}; // 录制前最后一次命令提交，不包含无关空闲栅栏。
    std::uint64_t frameIndex_{}; // 替换样本时保持递增的采集序号。
    unsigned capacity_{}; // 已分配时间戳容量，仅在 GPU 完成后增长。
    unsigned queryCount_{}; // 本次解析包含的已初始化查询槽数量。
    size_t expectedPassCount_{}; // 用于拒绝不完整录制的编译通道数量。
    bool recording_{}; // BeginFrame 与 EndFrame 之间的录制状态。
    bool activePass_{}; // 正在录制某个通道的屏障和回调。
    bool pending_{}; // 解析已录制但提交栅栏尚未允许回读。
};
}
