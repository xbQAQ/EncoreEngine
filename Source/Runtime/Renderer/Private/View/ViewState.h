#pragma once
// CPU 视图状态只消费场景快照与公共参数，不依赖帧调度实现。
#include "render/GpuTypes.h"
#include "gameplay/Components.h"

namespace render {
struct RenderSettings;

// 视图状态仅保留 CPU 相机与采样历史，GPU 常量由每个 Pass 独立声明和上传。
class ViewState {
public:
    void Begin(const gameplay::RenderSnapshot&, unsigned width, unsigned height, bool reset);
    FrameConstants Parameters() const;
    void Complete(RenderSettings&);
    unsigned FrameIndex() const { return frameIndex_; }
    unsigned Accumulated() const { return accumulated_; }
private:
    struct CameraFrame {
        Float4 position{}; // 当前相机世界位置。
        Float4 forward{}; // 前向单位向量及垂直半视场角正切。
        Float4 right{}; // 右向单位向量及图像宽高比。
        Float4 up{}; // 上向单位向量。
        Float4 resolution{}; // 视口宽高及各自倒数。
        Float4 projection{}; // 正数近远裁剪距离。
    };
    CameraFrame current_; // 本帧已准备的 CPU 相机数据。
    CameraFrame previous_; // 上次完成提交的 CPU 相机数据。
    unsigned frameIndex_{}; // 单调递增的随机采样序列索引。
    unsigned accumulated_{}; // 上次重置后的累计相机路径样本数。
    bool reset_{}; // 本帧视图是否应拒绝此前的累计样本。
};
}
