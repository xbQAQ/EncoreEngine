#include "render/Dx12Renderer.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef RENDER_SOURCE_DIRECTORY
#define RENDER_SOURCE_DIRECTORY L"."
#endif

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class HiddenWindow {
public:
    HiddenWindow() {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = DefWindowProcW;
        description.hInstance = instance_;
        description.lpszClassName = className_;
        Require(RegisterClassExW(&description) != 0, "Register editor import test window");
        // A real hidden HWND exercises the production swapchain and ImGui backend without interactive input.
        handle_ = CreateWindowExW(0, className_, L"Editor import integration", WS_OVERLAPPEDWINDOW,
            0, 0, 720, 540, nullptr, nullptr, instance_, nullptr);
        if (!handle_) {
            UnregisterClassW(className_, instance_);
            throw std::runtime_error("Create editor import test window");
        }
    }
    ~HiddenWindow() {
        if (handle_) DestroyWindow(handle_);
        UnregisterClassW(className_, instance_);
    }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    HWND Handle() const { return handle_; }

private:
    static constexpr const wchar_t* className_ = L"EncoreEditorImportIntegrationWindow"; // 独立窗口类隔离测试宿主，避免影响正式编辑器。
    HINSTANCE instance_{GetModuleHandleW(nullptr)}; // Executable instance owning the registered window class.
    HWND handle_{}; // Hidden host destroyed after the renderer and all swapchain resources.
};

void DrawTwoFrames(render::Dx12Renderer& renderer) {
    // Two frames exercise fresh histories followed by normal accumulation through the complete editor facade.
    for (unsigned frame = 0; frame < 2; ++frame) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        renderer.Frame();
    }
}

void CaptureScene(render::Dx12Renderer& renderer, const std::filesystem::path& path) {
    // CaptureService already rejects mostly blank pixel readbacks and restores the source resource state.
    renderer.Capture(path.wstring());
    Require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) > 128,
        "Scene capture must produce a nonempty PNG");
}
}

int main() {
    try {
        std::cout << std::unitbuf;
        const auto fixtures = std::filesystem::path(RENDER_SOURCE_DIRECTORY) / L"tests/fixtures";
        const auto valid = fixtures / L"textured_multilight.gltf";
        const auto invalid = fixtures / L"unsupported_required.gltf";
        Require(std::filesystem::is_regular_file(valid) && std::filesystem::is_regular_file(invalid),
            "Editor import fixtures must exist");
        const auto captures = std::filesystem::path(L"captures/editor-import");
        std::filesystem::create_directories(captures);
        HiddenWindow window;
        render::LaunchOptions options;
        options.width = options.height = 64;
        options.frames = 2;
        options.headless = true;
        options.validation = true;
        options.backend = 0;
        render::Dx12Renderer renderer(window.Handle(), options);
        DrawTwoFrames(renderer);
        CaptureScene(renderer, captures / L"cornell-before.png");

        // Replacement retires old graph resources while keeping the existing ImGui font/context alive.
        renderer.ImportScene(valid.wstring());
        DrawTwoFrames(renderer);
        CaptureScene(renderer, captures / L"imported.png");
        bool rejected = false;
        try { renderer.ImportScene(invalid.wstring()); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "An unsupported required extension must reject import");
        DrawTwoFrames(renderer);
        CaptureScene(renderer, captures / L"after-rejected-import.png");

        // Repeating a successful reload catches descriptor-cursor growth, stale histories and dangling pool references.
        renderer.ImportScene(valid.wstring());
        DrawTwoFrames(renderer);
        CaptureScene(renderer, captures / L"imported-again.png");
        const auto editorCapture = captures / L"editor-after-reload.png";
        renderer.CaptureEditor(editorCapture.wstring());
        Require(std::filesystem::is_regular_file(editorCapture) && std::filesystem::file_size(editorCapture) > 128,
            "ImGui composition must remain capturable after repeated scene replacement");
        std::cout << "PASS editor import: initial scene, reload, rejected import recovery, repeated reload and ImGui capture\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL editor import: " << error.what() << '\n';
        return 1;
    }
}
