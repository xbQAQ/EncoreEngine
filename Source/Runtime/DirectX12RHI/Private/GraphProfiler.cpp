#include "dx12/GraphProfiler.h"
#include "dx12/Dx12Device.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace render::dx12 {
namespace {
double Milliseconds(std::chrono::steady_clock::duration duration) {
    // CPU 时钟仅测量命令录制，绝不能替代 GPU 时间戳。
    return std::chrono::duration<double, std::milli>(duration).count();
}
}

GraphProfiler::GraphProfiler(Dx12Device& device) : device_(device) {
    // 直接队列提供全部 GPU 时间戳的真实频率换算。
    Check(device_.Queue()->GetTimestampFrequency(&timestampFrequency_), "Get graph timestamp frequency");
    if (!timestampFrequency_) throw std::runtime_error("The graph timestamp frequency is zero");
}

void GraphProfiler::EnsureCapacity(unsigned queryCount) {
    // 同步提交完成后可安全复用同一分配，无需额外保存帧环。
    if (capacity_ >= queryCount) return;
    unsigned capacity = std::max(64u, capacity_);
    while (capacity < queryCount) {
        if (capacity > std::numeric_limits<unsigned>::max() / 2) {
            capacity = queryCount;
            break;
        }
        capacity *= 2;
    }
    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDescription.Count = capacity;
    ComPtr<ID3D12QueryHeap> queries;
    Check(device_.Native()->CreateQueryHeap(&queryDescription, IID_PPV_ARGS(&queries)), "Create graph timestamp heap");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64(capacity) * sizeof(UINT64);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Check(device_.Native()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create graph timestamp readback");
    queries_ = std::move(queries);
    readback_ = std::move(readback);
    capacity_ = capacity;
}

void GraphProfiler::BeginFrame(size_t passCount, double cpuCompileMilliseconds) {
    // 命令列表仍在使用查询槽时不可覆盖。
    if (recording_ || (pending_ && !CollectCompleted()))
        throw std::logic_error("Graph profiler capture still has unfinished GPU work");
    if (passCount > (std::numeric_limits<unsigned>::max() - 2u) / 2u)
        throw std::length_error("Too many graph passes for timestamp queries");
    frameStart_ = Clock::now();
    queryCount_ = static_cast<unsigned>(passCount) * 2u + 2u;
    EnsureCapacity(queryCount_);
    latest_ = {};
    latest_.passes.reserve(passCount);
    latest_.cpuCompileMilliseconds = cpuCompileMilliseconds;
    latest_.gpuTimestampFrequency = timestampFrequency_;
    latest_.frameIndex = ++frameIndex_;
    expectedPassCount_ = passCount;
    priorSubmissionFence_ = device_.LastSubmissionFenceValue();
    recording_ = true;
    device_.Commands()->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
}

void GraphProfiler::BeginPass(const CompiledPass& pass) {
    // 在原生屏障前写入起点，将其 GPU 耗时归属消费通道。
    if (!recording_ || activePass_ || latest_.passes.size() >= expectedPassCount_)
        throw std::logic_error("Graph profiler pass begin is out of sequence");
    passStart_ = Clock::now();
    PassProfile sample;
    sample.name = pass.name;
    sample.cpuBeginMilliseconds = latest_.cpuCompileMilliseconds + Milliseconds(passStart_ - frameStart_);
    latest_.passes.push_back(std::move(sample));
    activePass_ = true;
    const unsigned query = 2u + static_cast<unsigned>(latest_.passes.size() - 1) * 2u;
    device_.Commands()->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
}

void GraphProfiler::EndPass() {
    // GPU 时间戳包围原生命令，CPU 时钟只覆盖录制行为。
    if (!recording_ || !activePass_) throw std::logic_error("Graph profiler pass end is out of sequence");
    const unsigned query = 3u + static_cast<unsigned>(latest_.passes.size() - 1) * 2u;
    device_.Commands()->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
    const auto end = Clock::now();
    latest_.passes.back().cpuMilliseconds = Milliseconds(end - passStart_);
    latest_.passes.back().cpuEndMilliseconds = latest_.cpuCompileMilliseconds + Milliseconds(end - frameStart_);
    activePass_ = false;
}

void GraphProfiler::RecordBarrier(double cpuMilliseconds) {
    // 最终导出屏障没有活动通道，但仍计入图总量。
    if (!recording_) throw std::logic_error("Graph profiler barrier is outside a capture");
    ++latest_.barrierCount;
    latest_.cpuBarrierMilliseconds += cpuMilliseconds;
    if (activePass_) {
        ++latest_.passes.back().barrierCount;
        latest_.passes.back().cpuBarrierMilliseconds += cpuMilliseconds;
    }
}

void GraphProfiler::EndFrame() {
    // 查询解析位于最终导出转换后、命令列表提交栅栏前。
    if (!recording_ || activePass_ || latest_.passes.size() != expectedPassCount_)
        throw std::logic_error("Graph profiler frame has incomplete pass timestamps");
    device_.Commands()->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    device_.Commands()->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
        queryCount_, readback_.Get(), 0);
    latest_.cpuMilliseconds = latest_.cpuCompileMilliseconds + Milliseconds(Clock::now() - frameStart_);
    recording_ = false;
    pending_ = true;
}

bool GraphProfiler::CollectCompleted() {
    // 空闲栅栏不能证明查询已执行，必须观察到更新的命令列表提交。
    if (!pending_) return false;
    const auto submission = device_.LastSubmissionFenceValue();
    if (submission <= priorSubmissionFence_ || device_.CompletedFenceValue() < submission) return false;
    const D3D12_RANGE range{0, SIZE_T(queryCount_) * sizeof(UINT64)};
    void* mapped = nullptr;
    Check(readback_->Map(0, &range, &mapped), "Map completed graph timestamps");
    const auto* begin = static_cast<const UINT64*>(mapped);
    const std::vector<UINT64> timestamps(begin, begin + queryCount_);
    const D3D12_RANGE written{0, 0};
    readback_->Unmap(0, &written);

    // 发布前校验解析范围，避免无效查询数据造成无符号减法下溢。
    if (timestamps[1] < timestamps[0]) throw std::runtime_error("Graph GPU timestamps are not monotonic");
    const double millisecondsPerTick = 1000.0 / static_cast<double>(timestampFrequency_);
    for (size_t index = 0; index < latest_.passes.size(); ++index) {
        const auto start = timestamps[index * 2 + 2];
        const auto end = timestamps[index * 2 + 3];
        if (start < timestamps[0] || end < start || end > timestamps[1])
            throw std::runtime_error("Graph pass GPU timestamps are outside the frame range");
        latest_.passes[index].gpuMilliseconds = static_cast<double>(end - start) * millisecondsPerTick;
        // 时间线由原始成对查询确定，不能累加 duration 伪造连续且无空隙的条形。
        latest_.passes[index].gpuBeginMilliseconds = static_cast<double>(start - timestamps[0]) * millisecondsPerTick;
        latest_.passes[index].gpuEndMilliseconds = static_cast<double>(end - timestamps[0]) * millisecondsPerTick;
    }
    latest_.gpuMilliseconds = static_cast<double>(timestamps[1] - timestamps[0]) * millisecondsPerTick;
    latest_.gpuAvailable = true;
    pending_ = false;
    return true;
}
}
