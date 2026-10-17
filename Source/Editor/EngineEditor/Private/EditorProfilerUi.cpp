#include "editor/EditorProfilerUi.h"
#include "EditorLocalization.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace render::editor {
namespace {
std::string LowerAscii(std::string value) {
    // UTF-8 中文保持原样，仅对英文阶段名进行大小写无关搜索。
    for (auto& letter : value) if (letter >= 'A' && letter <= 'Z') letter += 'a' - 'A';
    return value;
}

bool ValidInterval(double begin, double end) {
    return std::isfinite(begin) && std::isfinite(end) && begin >= 0 && end >= begin;
}

void PassTooltip(const PassProfile& pass) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(pass.name.c_str());
    // 起止使用基本中文字形，避免字体图集未包含箭头符号时显示缺字符方框。
    ImGui::Text("GPU：起 %.4f ms，止 %.4f ms，耗时 %.4f ms", pass.gpuBeginMilliseconds, pass.gpuEndMilliseconds, pass.gpuMilliseconds);
    ImGui::Text("CPU：起 %.4f ms，止 %.4f ms，耗时 %.4f ms", pass.cpuBeginMilliseconds, pass.cpuEndMilliseconds, pass.cpuMilliseconds);
    ImGui::Text("屏障：%u，CPU %.4f ms", pass.barrierCount, pass.cpuBarrierMilliseconds);
    ImGui::EndTooltip();
}
}

void EditorProfilerUi::Update(const GraphProfile& profile) {
    // 只接收本次会话的新完成帧；空闲时不复制 Pass 数据，也不缓存实时历史。
    lastObservedFrame_ = profile.frameIndex;
    if (!recording_ || !profile.gpuAvailable || !profile.frameIndex || profile.frameIndex == lastCapturedFrame_) return;
    frames_.push_back(profile);
    lastCapturedFrame_ = profile.frameIndex;
    selectedFrame_ = static_cast<int>(frames_.size());
    if (frames_.size() >= MaximumFrames) StopRecording();
}

void EditorProfilerUi::StartRecording() {
    // 每次开始建立独立会话，不能把停止期间保留的旧 GPU 样本算作新帧。
    Clear();
    recording_ = true;
}

void EditorProfilerUi::Clear() {
    // 清空只影响分析数据，不修改世界、渲染设置或作者操作历史。
    StopRecording();
    frames_.clear();
    lastCapturedFrame_ = lastObservedFrame_;
    selectedFrame_ = 1;
    selectedPass_.clear();
}
void EditorProfilerUi::Draw() {
    // 显式录制开关传回宿主；停止后保留原始样本，底层不再写时间戳。
    if (recording_) {
        if (ImGui::Button("停止录制###Stop Profile")) StopRecording();
    } else if (ImGui::Button("开始录制###Start Profile")) StartRecording();
    const auto nextControl = [](float width) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) ImGui::SameLine();
    };
    nextControl(ImGui::CalcTextSize("清空").x + ImGui::GetStyle().FramePadding.x * 2);
    ImGui::BeginDisabled(recording_ || frames_.empty());
    if (ImGui::Button("清空###Clear Profile")) Clear();
    ImGui::EndDisabled();
    ImGui::Text("%s · %zu/%zu 帧", recording_ ? "录制中" : frames_.empty() ? "未录制" : "已停止", frames_.size(), MaximumFrames);
    if (frames_.empty()) return;

    // 回看只改变选中帧，录制时禁用拖动以持续跟随最新的完整 GPU 样本。
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(recording_);
    ImGui::SliderInt("##Captured Frame", &selectedFrame_, 1, static_cast<int>(frames_.size()), "帧 %d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    const auto& sample = frames_.at(static_cast<size_t>(selectedFrame_ - 1));
    nextControl(55);
    if (ImGui::RadioButton("GPU###GPU Timeline", gpuTimeline_)) gpuTimeline_ = true;
    nextControl(55);
    if (ImGui::RadioButton("CPU###CPU Timeline", !gpuTimeline_)) gpuTimeline_ = false;
    ImGui::SetNextItemWidth(std::min(150.0f, std::max(50.0f, ImGui::GetContentRegionAvail().x - 60)));
    ImGui::DragFloat("缩放###Timeline Zoom", &zoom_, 0.1f, 1, 32, "%.1fx", ImGuiSliderFlags_AlwaysClamp);
    nextControl(ImGui::CalcTextSize("适应").x + ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::Button("适应###Fit Timeline")) zoom_ = 1;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##Pass Search", "搜索阶段", search_.data(), search_.size());
    ImGui::TextWrapped("GPU %.3f ms · CPU %.3f ms · 屏障 %u · 采样 %llu", sample.gpuMilliseconds,
        sample.cpuMilliseconds, sample.barrierCount, static_cast<unsigned long long>(sample.frameIndex));

    // 使用真实起止偏移保留 GPU 空隙；禁止按阶段耗时累加重建时间轴。
    double extent = gpuTimeline_ ? sample.gpuMilliseconds : sample.cpuMilliseconds;
    for (const auto& pass : sample.passes) {
        const auto begin = gpuTimeline_ ? pass.gpuBeginMilliseconds : pass.cpuBeginMilliseconds;
        const auto end = gpuTimeline_ ? pass.gpuEndMilliseconds : pass.cpuEndMilliseconds;
        if (ValidInterval(begin, end)) extent = std::max(extent, end);
    }
    if (!std::isfinite(extent) || extent <= 0) extent = 1;
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float labelWidth = std::min(180.0f, available * .35f);
    const float timelineWidth = std::max(60.0f, available - labelWidth - 28) * zoom_;
    const float scale = timelineWidth / static_cast<float>(extent);
    const auto filter = LowerAscii(search_.data());
    const auto flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("ProfileTimeline", 2, flags, ImVec2(0, std::max(1.0f, ImGui::GetContentRegionAvail().y)))) return;
    ImGui::TableSetupColumn("阶段", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labelWidth);
    ImGui::TableSetupColumn("时间（毫秒）", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, timelineWidth);
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(0);
    ImGui::TableHeader("阶段");
    ImGui::TableSetColumnIndex(1);
    auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto textColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const int divisions = std::clamp(static_cast<int>(timelineWidth / 90), 2, 64);
    // 表头刻度与阶段条使用同一个变换，横向滚动只移动时间列，阶段名保持可见。
    for (int tick = 0; tick <= divisions; ++tick) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%.2f", extent * tick / divisions);
        const float x = origin.x + timelineWidth * static_cast<float>(tick) / divisions;
        draw->AddText(ImVec2(std::min(x, origin.x + timelineWidth - ImGui::CalcTextSize(text).x), origin.y), textColor, text);
    }
    ImGui::Dummy(ImVec2(timelineWidth, ImGui::GetTextLineHeight()));
    constexpr std::array<ImU32, 6> colors{IM_COL32(59, 164, 180, 255), IM_COL32(205, 159, 67, 255),
        IM_COL32(122, 175, 102, 255), IM_COL32(194, 113, 125, 255), IM_COL32(144, 139, 203, 255), IM_COL32(107, 163, 212, 255)};
    unsigned row = 0;
    for (const auto& pass : sample.passes) {
        const auto* translated = EditorPassName(pass.name);
        const char* name = translated ? translated : pass.name.c_str();
        const auto color = colors[row++ % colors.size()];
        if (!filter.empty() && LowerAscii(pass.name + name).find(filter) == std::string::npos) continue;
        const bool selected = selectedPass_ == pass.name;
        const float rowHeight = std::max(22.0f, ImGui::GetTextLineHeightWithSpacing());
        ImGui::PushID(pass.name.c_str());
        ImGui::TableNextRow(0, rowHeight + ImGui::GetStyle().CellPadding.y * 2);
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable(name, selected, 0, ImVec2(0, rowHeight))) selectedPass_ = pass.name;
        if (ImGui::IsItemHovered()) PassTooltip(pass);
        ImGui::TableSetColumnIndex(1);
        origin = ImGui::GetCursorScreenPos();
        const auto begin = gpuTimeline_ ? pass.gpuBeginMilliseconds : pass.cpuBeginMilliseconds;
        const auto end = gpuTimeline_ ? pass.gpuEndMilliseconds : pass.cpuEndMilliseconds;
        // 整行注册稳定尺寸，绘图和命中测试只使用阶段真实区间，空隙没有虚假耗时。
        ImGui::InvisibleButton("##Pass Interval", ImVec2(timelineWidth, rowHeight));
        if (ValidInterval(begin, end)) {
            const ImVec2 minimum{origin.x + static_cast<float>(begin) * scale, origin.y + 2};
            const ImVec2 maximum{origin.x + std::max(static_cast<float>(end) * scale, static_cast<float>(begin) * scale + 2), origin.y + rowHeight - 2};
            draw->AddRectFilled(minimum, maximum, color);
            if (selected) draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Text), 0, 0, 2);
            if (ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(minimum, maximum)) {
                PassTooltip(pass);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selectedPass_ = pass.name;
            }
            char duration[32]{};
            std::snprintf(duration, sizeof(duration), "%.3f ms", end - begin);
            if (maximum.x - minimum.x >= ImGui::CalcTextSize(duration).x + 8) {
                draw->PushClipRect(minimum, maximum, true);
                draw->AddText(ImVec2(minimum.x + 4, origin.y + (rowHeight - ImGui::GetTextLineHeight()) * .5f),
                    IM_COL32(15, 20, 22, 255), duration);
                draw->PopClipRect();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
}
