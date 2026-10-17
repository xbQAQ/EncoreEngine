#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace render {
// 命令列表在录制后才提交，因此 CPU 录制与 GPU 执行时间必须分开统计。
struct PassProfile {
    std::string name; // 保留通道名称，顺序与编译后的执行顺序一致。
    double cpuMilliseconds{}; // 通道回调及其前置屏障的 CPU 录制耗时。
    double gpuMilliseconds{}; // 覆盖通道及前置屏障的真实 GPU 时间戳间隔。
    double cpuBarrierMilliseconds{}; // 转换和录制资源屏障的 CPU 耗时。
    unsigned barrierCount{}; // 此通道录制的原生状态转换和 UAV 屏障数量。
    double cpuBeginMilliseconds{}; // 相对图编译起点的 CPU 录制开始位置。
    double cpuEndMilliseconds{}; // 相对图编译起点的 CPU 录制结束位置。
    double gpuBeginMilliseconds{}; // 相对整图首个 GPU 时间戳的真实起点，保留通道间空隙。
    double gpuEndMilliseconds{}; // 相对整图首个 GPU 时间戳的真实终点。
};

struct GraphProfile {
    std::vector<PassProfile> passes; // 按执行顺序保存实际保留通道的样本。
    double cpuMilliseconds{}; // 图编译和录制总耗时，不包含栅栏等待。
    double gpuMilliseconds{}; // 首个图时间戳至最终导出屏障后的 GPU 耗时。
    double cpuCompileMilliseconds{}; // 验证、裁剪和调度渲染图的 CPU 耗时。
    double cpuBarrierMilliseconds{}; // 全部屏障的 CPU 录制耗时，包含最终导出转换。
    unsigned barrierCount{}; // 图中所有原生屏障数量，包含最终导出转换。
    std::uint64_t gpuTimestampFrequency{}; // GPU 时间换算使用的真实队列每秒时钟数。
    std::uint64_t frameIndex{}; // 此分析器单调递增的采集序号。
    bool gpuAvailable{}; // 仅在查询解析所属提交的栅栏完成后为真。
};
}
