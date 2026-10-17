#pragma once
#include "render/GraphProfile.h"
#include <array>
#include <string>

namespace render::editor {
class EditorProfilerUi {
public:
    void Update(const GraphProfile& profile);
    void Draw();
    void StopRecording() { recording_ = false; }
    bool Recording() const { return recording_; }
    const std::vector<GraphProfile>& CapturedFrames() const { return frames_; }
    static constexpr size_t MaximumFrames = 600;
    const std::string& SelectedPassName() const { return selectedPass_; }
private:
    void StartRecording();
    void Clear();
    std::vector<GraphProfile> frames_; // 仅保存当前录制会话的已完成帧，停止后保持可回看。
    std::uint64_t lastObservedFrame_{}; // 开始录制时排除上一次会话残留的运行时样本。
    std::uint64_t lastCapturedFrame_{}; // 同一完成样本多次送达时不重复计帧。
    int selectedFrame_{1}; // 停止后显示的一基帧编号，录制中跟随最新帧。
    bool recording_{}; // 同时驱动界面采样和宿主的真实 CPU/GPU 采集开关。
    bool gpuTimeline_{true}; // 在同一时间刻度上切换 GPU 执行与 CPU 录制区间。
    float zoom_{1}; // 相对于可见时间宽度的放大倍数，超出部分通过横向滚动查看。
    std::array<char, 256> search_{}; // 匹配原始阶段名或中文名称，不改变原始执行顺序。
    std::string selectedPass_; // 稳定阶段名让逐帧回看与筛选保持当前选择。
};
}
