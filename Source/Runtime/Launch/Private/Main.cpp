#include "render/Dx12Renderer.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
// ImGui 优先处理输入，宿主随后处理窗口寿命消息。
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

render::LaunchOptions ParseOptions(int argc, wchar_t** argv) {
    render::LaunchOptions options;
    // 拒绝未知设置，避免批处理静默使用错误的渲染算法。
    for (int i = 1; i < argc; ++i) {
        std::wstring argument = argv[i];
        const auto value = [&]() -> std::wstring {
            if (++i >= argc) throw std::runtime_error("Missing command-line value");
            return argv[i];
        };
        if (argument == L"--width") options.width = std::stoul(value());
        else if (argument == L"--height") options.height = std::stoul(value());
        else if (argument == L"--frames") options.frames = std::stoul(value());
        else if (argument == L"--spp") options.samples = std::stoul(value());
        else if (argument == L"--capture") options.capture = value();
        else if (argument == L"--capture-ui") options.editorCapture = value();
        else if (argument == L"--scene") options.scenePath = value();
        else if (argument == L"--headless") options.headless = true;
        else if (argument == L"--validation") options.validation = true;
        else if (argument == L"--pipeline") {
            const auto mode = value();
            if (mode == L"raster") options.pipeline = 0;
            else if (mode == L"raytracing") options.pipeline = 1;
            else if (mode == L"dxr") { options.pipeline = 1; options.backend = 1; options.gi = 0; }
            else throw std::runtime_error("Pipeline must be raster, raytracing, or dxr");
        } else if (argument == L"--gi") {
            const auto mode = value();
            if (mode == L"pathtracing") options.gi = 0;
            else if (mode == L"ssgi") options.gi = 1;
            else if (mode == L"restir") options.gi = 2;
            else throw std::runtime_error("GI must be pathtracing, ssgi, or restir");
        } else if (argument == L"--shadow") {
            const auto mode = value();
            if (mode == L"rays") options.shadowMode = 0;
            else if (mode == L"pcf") options.shadowMode = 1;
            else if (mode == L"pcss") options.shadowMode = 2;
            else if (mode == L"vsm") options.shadowMode = 3;
            else throw std::runtime_error("Shadow must be rays, pcf, pcss, or vsm");
        } else if (argument == L"--backend") {
            const auto mode = value();
            if (mode == L"software") options.backend = 0;
            else if (mode == L"dxr") options.backend = 1;
            else throw std::runtime_error("Backend must be software or dxr");
        } else throw std::runtime_error("Unknown command-line option");
    }
    // 未显式指定管线时，兼容旧版 GI 和求交后端命令的光追语义。
    bool explicitPipeline = false, legacyGi = false;
    for (int i = 1; i < argc; ++i) {
        explicitPipeline |= std::wstring_view(argv[i]) == L"--pipeline";
        legacyGi |= std::wstring_view(argv[i]) == L"--gi" || std::wstring_view(argv[i]) == L"--backend";
    }
    if (!explicitPipeline && legacyGi) options.pipeline = 1;
    if (options.width < 64 || options.height < 64 || options.width > 3840 || options.height > 2160)
        throw std::runtime_error("Render extent must be between 64x64 and 3840x2160");
    // 输入上限必须与着色器采样循环及累积历史的分母一致。
    if (options.samples < 1 || options.samples > 16) throw std::runtime_error("SPP must be 1..16");
    return options;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = ParseOptions(argc, argv);
        // 批处理使用隐藏真实窗口，保证交换链与交互运行遵循相同路径。
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        // 窗口类与诊断前缀共享引擎名称，避免与改名前仍在运行的编辑器混淆。
        windowClass.lpszClassName = L"EncoreEngineWindow";
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&windowClass);
        // 原生窗口标题与 UTF-8 编辑器叠加层统一使用中文。
        const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"EncoreEngine 引擎编辑器 | DirectX 12",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1360, 840, nullptr, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("CreateWindowEx failed");
        if (!options.headless) ShowWindow(window, SW_SHOWDEFAULT);
        render::Dx12Renderer renderer(window, options);
        unsigned rendered = 0;
        bool running = true;
        while (running && (!options.frames || rendered < options.frames)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) running = false;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;
            renderer.Frame();
            ++rendered;
        }
        if (!options.capture.empty()) renderer.Capture(options.capture);
        if (!options.editorCapture.empty()) renderer.CaptureEditor(options.editorCapture);
        std::cout << "Completed " << rendered << " frames\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "EncoreEngine: " << error.what() << '\n';
        return 1;
    }
}
