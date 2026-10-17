#include "render/Dx12Renderer.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class IsolatedLayout {
public:
    IsolatedLayout() {
        // 仅此测试进程替换布局目录，真实编辑器的持久化文件不会被读取或覆盖。
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous_.data(), static_cast<DWORD>(previous_.size()));
        Require(length < previous_.size(), "Read original local application directory");
        hadPrevious_ = length != 0;
        // 引擎前缀与进程、时钟共同隔离临时布局，避免读取旧版本的持久化状态。
        directory_ = std::filesystem::temp_directory_path() /
            (L"EncorePlatformTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(directory_);
        Require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory_.c_str()) != 0, "Isolate native editor layout");
    }
    ~IsolatedLayout() {
        SetEnvironmentVariableW(L"LOCALAPPDATA", hadPrevious_ ? previous_.data() : nullptr);
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
private:
    std::array<wchar_t, 32768> previous_{}; // 本进程原有目录，测试退出时恢复。
    std::filesystem::path directory_; // 带进程号和时钟后缀的唯一临时布局根目录。
    bool hadPrevious_{}; // 区分未设置变量与非空的原始目录。
};

class TestWindow {
public:
    TestWindow() {
        // 主窗口保持隐藏，测试只短暂显示用户要求验证的独立原生工具窗口。
        handle_ = CreateWindowExW(0, L"STATIC", L"Editor platform validation", WS_OVERLAPPEDWINDOW,
            20, 20, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(handle_ != nullptr, "Create native editor host");
    }
    ~TestWindow() { if (handle_) DestroyWindow(handle_); }
    HWND Handle() const { return handle_; }
private:
    HWND handle_{}; // 渲染宿主和 ImGui 平台后端销毁后才释放的主窗口。
};

struct SyntheticInput {
    ImVec2 mouse{-10000, -10000}; // 绝对屏幕坐标，输入不移动系统鼠标。
    bool pressed{}; // 仅向测试 ImGui 上下文注入的左键状态。
    bool shift{}; // 拖出时禁止自动重新停靠，保证目标位置确定。
};

bool HasRenderedPixels(HWND window, const ImRect* screenRegion = nullptr) {
    // 只前置本进程的测试窗口，并保留图像相对客户区的位置，避免移动后采样错位。
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    Require(process == GetCurrentProcessId(), "Pixel validation may only reposition its own test window");
    RECT client{};
    Require(GetClientRect(window, &client) && client.right > 0 && client.bottom > 0,
        "Read a nonempty native client area");
    POINT origin{};
    Require(ClientToScreen(window, &origin) != 0, "Locate native client area before sampling");
    const LONG left = screenRegion ? std::clamp(static_cast<LONG>(screenRegion->Min.x) - origin.x + 4, 0L, client.right) : 16;
    const LONG top = screenRegion ? std::clamp(static_cast<LONG>(screenRegion->Min.y) - origin.y + 4, 0L, client.bottom) : 16;
    const LONG right = screenRegion ? std::clamp(static_cast<LONG>(screenRegion->Max.x) - origin.x - 4, 0L, client.right) : client.right - 16;
    const LONG bottom = screenRegion ? std::clamp(static_cast<LONG>(screenRegion->Max.y) - origin.y - 4, 0L, client.bottom) : client.bottom - 16;
    Require(left < right && top < bottom, "Native pixel sample area must not be empty");
    RECT position{};
    MONITORINFO monitor{sizeof(MONITORINFO)};
    Require(GetWindowRect(window, &position) && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor),
        "Locate the test window monitor");
    const auto x = std::clamp(position.left, monitor.rcWork.left,
        std::max(monitor.rcWork.left, monitor.rcWork.right - (position.right - position.left)));
    const auto y = std::clamp(position.top, monitor.rcWork.top,
        std::max(monitor.rcWork.top, monitor.rcWork.bottom - (position.bottom - position.top)));
    Require(SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != 0,
        "Bring only the native test window forward for pixel validation");
    DwmFlush();
    // ClientToScreen 原地变换输入；重定位后必须重新从客户区原点开始，不能累加上次屏幕坐标。
    origin = {};
    Require(ClientToScreen(window, &origin) != 0, "Locate the visible native client area");
    // 每个采样点都必须在真实屏幕内，且最上层命中窗口属于目标 HWND，不能把遮挡物当作渲染结果。
    const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const auto owner = GetAncestor(window, GA_ROOT);
    const auto visibleGrid = [&] {
        if (!IsWindowVisible(window) || IsIconic(window)) return false;
        for (LONG row = top; row < bottom; row += 3)
            for (LONG column = left; column < right; column += 3) {
                const POINT point{origin.x + column, origin.y + row};
                if (!PtInRect(&desktop, point)) {
                    std::cerr << "Native sample outside desktop: " << point.x << ',' << point.y << '\n';
                    return false;
                }
                const auto hit = WindowFromPoint(point);
                if (!hit || GetAncestor(hit, GA_ROOT) != owner) {
                    // 首个不匹配点记录真实命中窗口，区分坐标错误、透明命中与其它窗口遮挡。
                    std::array<char, 256> hitClass{};
                    GetClassNameA(hit, hitClass.data(), static_cast<int>(hitClass.size()));
                    DWORD hitProcess{};
                    GetWindowThreadProcessId(hit, &hitProcess);
                    RECT bounds{}, hitBounds{};
                    GetWindowRect(window, &bounds);
                    GetWindowRect(GetAncestor(hit, GA_ROOT), &hitBounds);
                    std::cerr << "Native sample ownership: point=" << point.x << ',' << point.y
                        << ", target=" << window << ", root=" << owner << ", hit=" << hit
                        << ", hitRoot=" << GetAncestor(hit, GA_ROOT) << ", hitClass=" << hitClass.data()
                        << ", hitProcess=" << hitProcess << ", targetStyle=" << GetWindowLongPtrW(window, GWL_STYLE)
                        << ", targetExStyle=" << GetWindowLongPtrW(window, GWL_EXSTYLE)
                        << ", clientOrigin=" << origin.x << ',' << origin.y << ", targetBounds="
                        << bounds.left << ',' << bounds.top << ',' << bounds.right << ',' << bounds.bottom
                        << ", hitBounds=" << hitBounds.left << ',' << hitBounds.top << ',' << hitBounds.right << ',' << hitBounds.bottom
                        << ", dpi=" << GetDpiForWindow(window) << '\n';
                    return false;
                }
            }
        return true;
    };
    const bool ownedBefore = visibleGrid();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    // Flip-model 交换链不回填窗口自身的 GDI 位图，应从桌面合成面按客户区屏幕坐标读取。
    HDC source = GetDC(nullptr), target = CreateCompatibleDC(source);
    void* bytes{};
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bytes, nullptr, 0);
    const auto previous = bitmap ? SelectObject(target, bitmap) : nullptr;
    const bool copied = ownedBefore && bitmap && BitBlt(target, 0, 0, client.right, client.bottom, source, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    const bool ownedAfter = copied && visibleGrid();
    std::unordered_set<unsigned> colors;
    if (ownedAfter) {
        const auto* pixels = static_cast<const unsigned*>(bytes);
        // 场景验证仅采样图像内部，排除工具栏文字让空白渲染误通过的可能。
        for (LONG row = top; row < bottom && colors.size() <= 32; row += 3)
            for (LONG column = left; column < right && colors.size() <= 32; column += 3)
                colors.insert(pixels[static_cast<size_t>(row) * client.right + column] & 0x00ffffff);
    }
    // 失败时保留当前客户区位图，便于区分后端空白与桌面截图不可用。
    if (colors.size() <= 32) {
        GetWindowRect(window, &position);
        std::cerr << "Native pixels: " << client.right << 'x' << client.bottom << ", copied=" << copied << ", colors=" << colors.size()
            << ", ownedBefore=" << ownedBefore << ", ownedAfter=" << ownedAfter
            << ", visible=" << IsWindowVisible(window) << ", position=" << position.left << ',' << position.top
            << ", screen=" << GetSystemMetrics(SM_CXSCREEN) << 'x' << GetSystemMetrics(SM_CYSCREEN) << '\n';
        if (copied) {
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
            header.bfSize = header.bfOffBits + client.right * client.bottom * 4;
            std::ofstream file("editor-platform-window.bmp", std::ios::binary);
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
            file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(client.right) * client.bottom * 4);
        }
    }
    if (previous) SelectObject(target, previous);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(target);
    ReleaseDC(nullptr, source);
    return colors.size() > 32;
}
}

int main() {
    try {
        IsolatedLayout layout;
        TestWindow window;
        SyntheticInput input;
        render::LaunchOptions options;
        options.width = options.height = 64;
        options.frames = 1;
        options.validation = true;
        options.headless = false;
        render::Dx12Renderer renderer(window.Handle(), options);
        auto& io = ImGui::GetIO();
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigViewportsNoTaskBarIcon = true;
        Require((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0, "Interactive editor enables native viewports");
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePre;
        hook.UserData = &input;
        hook.Callback = [](ImGuiContext*, ImGuiContextHook* current) {
            // 在系统轮询之后追加合成事件，真实 Build、停靠状态机和 GPU 宿主保持不变。
            const auto& state = *static_cast<const SyntheticInput*>(current->UserData);
            auto& events = ImGui::GetIO();
            events.AddFocusEvent(true);
            events.AddMousePosEvent(state.mouse.x, state.mouse.y);
            events.AddMouseViewportEvent(0);
            events.AddMouseButtonEvent(ImGuiMouseButton_Left, state.pressed);
            events.AddKeyEvent(ImGuiMod_Shift, state.shift);
        };
        ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
        auto frame = [&](ImVec2 mouse = {-10000, -10000}, bool pressed = false) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            input.mouse = mouse;
            input.pressed = pressed;
            renderer.Frame();
        };
        frame(); frame(); frame();
        auto* profile = ImGui::FindWindowByName("###Profile");
        Require(profile && profile->DockNode && profile->DockNode->TabBar, "Profiler starts in an independent dock tab");
        auto* mainViewport = ImGui::GetMainViewport();
        ImVec2 tabCenter;
        for (const auto& tab : profile->DockNode->TabBar->Tabs) if (tab.ID == profile->TabId)
            tabCenter = {profile->DockNode->TabBar->BarRect.Min.x + tab.Offset + tab.Width * .5f,
                profile->DockNode->TabBar->BarRect.GetCenter().y};
        const ImVec2 destination{mainViewport->Pos.x + mainViewport->Size.x + 100, mainViewport->Pos.y + 150};
        // 真实鼠标拖动将标签移出主 HWND；随后帧由正式宿主创建、渲染和呈现平台交换链。
        frame(tabCenter); frame(tabCenter); frame(tabCenter, true);
        input.shift = true;
        frame(destination, true); frame(destination, true); frame(destination); frame(destination);
        input.shift = false;
        frame(); frame(); frame();
        Require(!profile->DockNode && profile->Viewport != mainViewport && ImGui::GetPlatformIO().Viewports.Size >= 2,
            "Dragging a tool outside the host creates an independent native viewport");
        const auto platformHandle = static_cast<HWND>(profile->Viewport->PlatformHandle);
        Require(platformHandle && platformHandle != window.Handle() && IsWindow(platformHandle) &&
            profile->Viewport->RendererUserData && profile->Viewport->DrawData && profile->Viewport->DrawData->TotalVtxCount > 0,
            "Detached profiler owns a live native window, renderer state and draw data");
        Require(HasRenderedPixels(platformHandle), "Native profiler client area contains actual rendered text and timeline pixels");

        // 原生边框缩放反馈到 ImGui，并触发后端交换链缓冲重建和后续绘制。
        RECT original{};
        GetClientRect(platformHandle, &original);
        SetWindowPos(platformHandle, nullptr, 0, 0, original.right + 120, original.bottom + 90,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        frame(); frame(); frame();
        RECT resized{};
        GetClientRect(platformHandle, &resized);
        Require(resized.right >= original.right + 80 && resized.bottom >= original.bottom + 50 && HasRenderedPixels(platformHandle),
            "Resized native tool keeps rendering through its independent swapchain");

        // 正式停靠 API 收回工具后，平台更新销毁旧 HWND 和交换链；其余主帧继续绘制。
        auto* files = ImGui::FindWindowByName("###ContentBrowser");
        ImGui::DockBuilderDockWindow("性能时间线###Profile", files->DockId);
        frame(); frame(); frame();
        Require(profile->DockNode && profile->Viewport == mainViewport && !IsWindow(platformHandle) &&
            ImGui::GetPlatformIO().Viewports.Size == 1, "Redocking destroys the old native viewport and returns to the main host");

        // 再拖出真正读取 Display SRV 的场景视口，验证主图完成后的跨队列纹理读取与像素内容。
        auto* scene = ImGui::FindWindowByName("###Viewport");
        for (const auto& tab : scene->DockNode->TabBar->Tabs) if (tab.ID == scene->TabId)
            tabCenter = {scene->DockNode->TabBar->BarRect.Min.x + tab.Offset + tab.Width * .5f,
                scene->DockNode->TabBar->BarRect.GetCenter().y};
        frame(tabCenter); frame(tabCenter); frame(tabCenter, true);
        input.shift = true;
        frame(destination, true); frame(destination, true); frame(destination); frame(destination);
        input.shift = false;
        frame(); frame(); frame();
        Require(!scene->DockNode && scene->Viewport != mainViewport, "Scene viewport can detach into its own native window");
        ImRect image(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
        for (const auto& command : scene->DrawList->CmdBuffer) {
            if (command.GetTexID() == io.Fonts->TexID || !command.ElemCount) continue;
            for (unsigned index = 0; index < command.ElemCount; ++index)
                image.Add(scene->DrawList->VtxBuffer[command.VtxOffset + scene->DrawList->IdxBuffer[command.IdxOffset + index]].pos);
        }
        Require(!image.IsInverted() && image.GetWidth() > 100 && image.GetHeight() > 100 &&
            HasRenderedPixels(static_cast<HWND>(scene->Viewport->PlatformHandle), &image),
            "Detached scene displays nonblank shared Display pixels after main-queue completion");
        std::cout << "PASS native editor windows: drag outside host, independent DX12 presentation, rendered pixels, resize and redock destruction\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL editor platform windows: " << error.what() << '\n';
        return 1;
    }
}
