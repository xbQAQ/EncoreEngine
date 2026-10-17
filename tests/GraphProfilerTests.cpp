#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/GraphProfiler.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
    // 显式抛错使 Release 中的 GPU 验证失败同样反馈给 CTest。
    if (!condition) throw std::runtime_error(message);
}

render::dx12::GpuResource Buffer(render::dx12::Dx12Device& device, const char* name,
    UINT64 bytes, D3D12_RESOURCE_STATES state) {
    // 原生缓冲工作负载独立于渲染着色器与场景准备。
    render::dx12::GpuResource result;
    result.name = name;
    result.state = state;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    render::dx12::Check(device.Native()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &description, state, nullptr, IID_PPV_ARGS(&result.object)), "Create profiler workload buffer");
    return result;
}
}

int main() {
    try {
        // CPU 休眠和真实 GPU 复制分离两个时钟，防止 CPU 估算冒充 GPU 耗时。
        render::dx12::Dx12Device device(true);
        render::dx12::GraphProfiler profiler(device);
        constexpr UINT64 bytes = 16 * 1024 * 1024;
        // 现代 D3D12 缓冲从 COMMON 开始，由 RDG 显式建立两侧复制状态。
        auto source = Buffer(device, "Profile.Source", bytes, D3D12_RESOURCE_STATE_COMMON);
        auto destination = Buffer(device, "Profile.Destination", bytes, D3D12_RESOURCE_STATE_COMMON);
        device.BeginCommands();
        render::RenderGraph graph;
        render::dx12::GraphExecutor executor(device, &profiler);
        executor.Import(graph, source);
        executor.Import(graph, destination);
        graph.AddPass("Profile.CpuRecording", {}, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }, true);
        graph.AddPass("Profile.GpuCopies", {
            render::dx12::Read(source, D3D12_RESOURCE_STATE_COPY_SOURCE),
            render::dx12::Write(destination, D3D12_RESOURCE_STATE_COPY_DEST)}, [&] {
            for (unsigned copy = 0; copy < 16; ++copy)
                device.Commands()->CopyBufferRegion(destination.object.Get(), 0, source.object.Get(), 0, bytes);
        });
        graph.Export(destination.graphId, D3D12_RESOURCE_STATE_COPY_DEST);
        executor.Execute(graph);
        Require(!profiler.Latest().gpuAvailable, "GPU results must remain unavailable during recording");
        Require(!profiler.CollectCompleted(), "An unsubmitted graph must not map its timestamp readback");

        // 空闲栅栏不能证明此录制命令列表确实已经提交。
        device.WaitIdle();
        Require(!profiler.CollectCompleted(), "An unrelated idle fence must not publish graph timings");
        device.SubmitAndWait();
        Require(profiler.CollectCompleted(), "A completed graph submission must publish GPU timings");
        const auto first = profiler.Latest();
        Require(first.gpuAvailable && first.gpuTimestampFrequency > 0, "GPU timings require the actual queue frequency");
        Require(first.passes.size() == 2, "Only retained graph passes may appear in the profile");
        Require(first.passes[0].cpuMilliseconds >= 20, "CPU recording must include callback recording cost");
        Require(first.passes[0].gpuMilliseconds < first.passes[0].cpuMilliseconds * 0.5,
            "CPU recording delay must not be reported as GPU execution time");
        Require(first.passes[1].gpuMilliseconds > 0, "Real GPU copy work must produce timestamp duration");
        Require(first.barrierCount == 2 && first.passes[1].barrierCount == 2,
            "Pass resource transitions must contribute to barrier statistics");
        double sum = 0;
        for (const auto& pass : first.passes) {
            // 起止位置必须与实际耗时一致，且落在同一图的采样范围内。
            Require(pass.cpuBeginMilliseconds >= first.cpuCompileMilliseconds && pass.cpuEndMilliseconds <= first.cpuMilliseconds &&
                std::abs(pass.cpuEndMilliseconds - pass.cpuBeginMilliseconds - pass.cpuMilliseconds) < 0.00001,
                "CPU timeline positions must match actual recording intervals");
            Require(pass.gpuBeginMilliseconds >= 0 && pass.gpuEndMilliseconds <= first.gpuMilliseconds &&
                std::abs(pass.gpuEndMilliseconds - pass.gpuBeginMilliseconds - pass.gpuMilliseconds) < 0.00001,
                "GPU timeline positions must come from actual graph-relative timestamps");
            Require(std::isfinite(pass.gpuMilliseconds) && pass.gpuMilliseconds >= 0,
                "Every GPU duration must be finite and nonnegative");
            Require(std::isfinite(pass.cpuMilliseconds) && pass.cpuMilliseconds >= 0,
                "Every CPU duration must be finite and nonnegative");
            sum += pass.gpuMilliseconds;
        }
        Require(first.gpuMilliseconds >= sum - 0.000001, "The whole graph must include every pass GPU duration");

        // 查询分配复用后，旧可用状态必须失效直到新提交完成。
        device.BeginCommands();
        render::RenderGraph empty;
        render::dx12::GraphExecutor secondExecutor(device, &profiler);
        secondExecutor.Execute(empty);
        Require(!profiler.Latest().gpuAvailable && !profiler.CollectCompleted(),
            "The prior frame fence must not expose overwritten timestamp queries");
        device.SubmitAndWait();
        Require(profiler.CollectCompleted(), "An empty graph must still complete its timestamp range");
        Require(profiler.Latest().frameIndex > first.frameIndex && profiler.Latest().passes.empty(),
            "Empty graphs must replace prior pass samples without losing frame sequence");
        Require(profiler.Latest().gpuMilliseconds >= 0, "Empty graph duration must remain nonnegative");
        std::cout << "GPU profiler passed: " << device.AdapterName() << ", queue "
            << first.gpuTimestampFrequency << " Hz, copies " << first.passes[1].gpuMilliseconds
            << " ms, CPU-only recording " << first.passes[0].cpuMilliseconds << " ms\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
