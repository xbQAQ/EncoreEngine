#include "editor/EditorProfilerUi.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ImRect ColoredBar(ImU32 color) {
    // 直接读取正式绘制顶点，验证可见时间条的位置，而不是重复被测时间换算函数。
    ImRect result(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
    const auto* data = ImGui::GetDrawData();
    for (int list = 0; list < data->CmdListsCount; ++list)
        for (const auto& vertex : data->CmdLists[list]->VtxBuffer)
            if (vertex.col == color) result.Add(vertex.pos);
    return result;
}
}

int main() {
    try {
        // 测试隔离于用户布局和原生窗口，以真实 ImGui 输入运行同一时间线代码。
        std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1000, 600};
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        Require(io.Fonts->Build(), "Build CPU-only ImGui atlas");
        render::editor::EditorProfilerUi profiler;
        render::GraphProfile sample;
        sample.gpuAvailable = true;
        sample.gpuMilliseconds = 10;
        sample.cpuMilliseconds = 8;
        sample.frameIndex = 7;
        render::PassProfile first;
        first.name = "Early.Pass";
        first.gpuBeginMilliseconds = 2; first.gpuEndMilliseconds = 3; first.gpuMilliseconds = 1;
        first.cpuBeginMilliseconds = 1; first.cpuEndMilliseconds = 2; first.cpuMilliseconds = 1;
        first.barrierCount = 3;
        render::PassProfile second;
        second.name = "Late.Pass";
        second.gpuBeginMilliseconds = 8; second.gpuEndMilliseconds = 9; second.gpuMilliseconds = 1;
        second.cpuBeginMilliseconds = 5; second.cpuEndMilliseconds = 7; second.cpuMilliseconds = 2;
        sample.passes = {first, second};
        profiler.Update(sample);
        ImVec2 size{1000, 600};
        auto frame = [&](ImVec2 mouse = {-100, -100}, bool down = false) {
            io.DeltaTime = 1.0f / 60;
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize(size);
            ImGui::Begin("Profiler Test");
            profiler.Draw();
            ImGui::End();
            ImGui::Render();
        };
        frame(); frame(); frame();
        // 空闲状态不能默默接收运行时样本；只有显式开始录制才应产生时间条。
        Require(ColoredBar(IM_COL32(59, 164, 180, 255)).IsInverted(),
            "Idle profiler must ignore live samples until recording is explicitly started");
        auto* window = ImGui::FindWindowByName("Profiler Test");
        // 用真实按钮启动与停止会话，重复发布、未完成查询和停止后的实时数据都不能混入结果。
        ImGui::ActivateItemByID(window->GetID("###Start Profile")); frame(); frame();
        Require(profiler.Recording(), "Start button must enable the renderer capture request");
        profiler.Update(sample);
        Require(profiler.CapturedFrames().empty(), "Starting a session must exclude a retained sample from the previous session");
        ++sample.frameIndex;
        sample.gpuAvailable = false;
        profiler.Update(sample);
        Require(profiler.CapturedFrames().empty(), "An incomplete GPU sample must not enter the capture");
        sample.gpuAvailable = true;
        profiler.Update(sample); profiler.Update(sample);
        Require(profiler.CapturedFrames().size() == 1, "Repeated delivery of a completed frame must be deduplicated");
        ++sample.frameIndex;
        sample.passes[1].cpuBeginMilliseconds = 3;
        sample.passes[1].cpuMilliseconds = 4;
        profiler.Update(sample); frame(); frame();
        ImGui::ActivateItemByID(window->GetID("###Stop Profile")); frame(); frame();
        Require(!profiler.Recording() && profiler.CapturedFrames().size() == 2, "Stopping must disable sampling while retaining both recorded frames");
        ++sample.frameIndex;
        profiler.Update(sample);
        Require(profiler.CapturedFrames().size() == 2, "Stopped captures must ignore subsequent runtime updates");

        // 使用正式滑块的数值输入回看旧帧，验证保存的是各帧独立值而不是最新样本的引用。
        auto enterNumber = [&](ImGuiID id, const char* text) {
            ImGui::ActivateItemByID(id);
            ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
            frame();
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
            io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiKey_A, false);
            io.AddInputCharactersUTF8(text); frame();
            io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame(); frame();
        };
        enterNumber(window->GetID("##Captured Frame"), "1");
        auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(window->GetID("ProfileTimeline"));
        Require(table && table->InnerWindow && table->FreezeColumnsRequest == 1, "Timeline is a scrolling table with frozen pass names");
        constexpr ImU32 firstColor = IM_COL32(59, 164, 180, 255), secondColor = IM_COL32(205, 159, 67, 255);
        auto early = ColoredBar(firstColor), late = ColoredBar(secondColor);
        const float unit = early.GetWidth();
        Require(unit > 30 && std::abs(late.GetWidth() - unit) < 1 && std::abs(late.Min.x - early.Max.x - unit * 5) < 2,
            "GPU bars retain the real five-millisecond gap rather than accumulating pass durations");
        Require(std::abs(early.Min.x - table->Columns[1].WorkMinX - unit * 2) < 2,
            "GPU timeline preserves idle time before the first pass");

        // 条形命中必须改变正式选择，悬停显示阶段起止、耗时和屏障信息。
        const auto center = late.GetCenter();
        frame(center); frame(center, true); frame(center);
        Require(profiler.SelectedPassName() == second.name, "Pointer input selects the hovered pass interval");
        for (int count = 0; count < 40; ++count) frame(center);
        bool tooltip = false;
        for (auto* candidate : ImGui::GetCurrentContext()->Windows)
            if (candidate->Active && (candidate->Flags & ImGuiWindowFlags_Tooltip)) tooltip = true;
        Require(tooltip, "Hovering a real pass bar shows a tooltip");
        ImGui::ActivateItemByID(window->GetID("###CPU Timeline")); frame(); frame();
        early = ColoredBar(firstColor); late = ColoredBar(secondColor);
        Require(std::abs(late.GetWidth() - early.GetWidth() * 2) < 2 &&
            std::abs(late.Min.x - early.Max.x - early.GetWidth() * 3) < 2,
            "CPU mode uses its own recording offsets and durations");

        // 通过真实数值控件输入放大倍数，再检查横向滚动与冻结列的实际行为。
        enterNumber(window->GetID("###Timeline Zoom"), "4");
        Require(table->InnerWindow->ScrollMax.x > 1000, "Timeline zoom creates a usable horizontal scroll range");
        const float labelX = table->Columns[0].MinX;
        ImGui::SetScrollX(table->InnerWindow, 250); frame(); frame();
        Require(table->InnerWindow->Scroll.x > 200 && std::abs(table->Columns[0].MinX - labelX) < 1,
            "Horizontal scrolling preserves the frozen pass-name column");
        ImGui::ActivateItemByID(window->GetID("###Fit Timeline")); frame(); frame(); frame();
        Require(table->InnerWindow->ScrollMax.x < 2, "Fit resets zoom and removes unnecessary horizontal scrolling");

        // 搜索只隐藏不匹配行；停止录制后可以在两个保存的帧之间反复回看。
        ImGui::ActivateItemByID(window->GetID("##Pass Search")); frame();
        io.AddInputCharactersUTF8("LATE"); frame(); frame();
        Require(ColoredBar(firstColor).IsInverted() && !ColoredBar(secondColor).IsInverted(), "Pass search is case insensitive and keeps the matching interval");
        io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        const auto frozenBar = ColoredBar(secondColor);
        profiler.Update(sample); frame();
        Require(std::abs(ColoredBar(secondColor).Min.x - frozenBar.Min.x) < 1, "Stopped samples remain stable while runtime updates continue");
        enterNumber(window->GetID("##Captured Frame"), "2");
        Require(ColoredBar(secondColor).Min.x < frozenBar.Min.x - 50, "Frame selection must display the chosen capture instead of the latest runtime sample");

        // 紧凑窗口保持自身无第二层垂直滚动条；清空、重新开始和达到预算上限均使用同一会话状态。
        size = {320, 300}; frame(); frame(); frame();
        Require(!ColoredBar(secondColor).IsInverted() && window->ScrollMax.y == 0, "Recorded CPU timeline must fit compact panels");
        ImGui::ActivateItemByID(window->GetID("###Clear Profile")); frame(); frame();
        Require(profiler.CapturedFrames().empty() && !profiler.Recording(), "Clear must release recorded data without enabling collection");
        ImGui::ActivateItemByID(window->GetID("###Start Profile")); frame(); frame();
        profiler.Update(sample);
        Require(profiler.Recording() && profiler.CapturedFrames().empty(), "Restart must exclude the stopped session's stale sample");
        for (size_t index = 0; index < render::editor::EditorProfilerUi::MaximumFrames; ++index) {
            ++sample.frameIndex;
            profiler.Update(sample);
        }
        Require(!profiler.Recording() && profiler.CapturedFrames().size() == render::editor::EditorProfilerUi::MaximumFrames,
            "The capture budget must stop recording instead of accumulating unbounded history");
        ++sample.frameIndex; profiler.Update(sample);
        Require(profiler.CapturedFrames().size() == render::editor::EditorProfilerUi::MaximumFrames,
            "Frames arriving after the capture limit must be ignored");
        std::cout << "PASS profiler: explicit recording, idle/stop isolation, frame history and limit, real intervals, CPU/GPU modes, selection, hover, zoom, scroll, fit and search\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL editor profiler: " << error.what() << '\n';
        return 1;
    }
}
