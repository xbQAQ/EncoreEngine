// 宿主测试组合私有帧入口，扩展模块仍只消费公共管线契约。
#include "Frame/FrameRenderer.h"
#include "modules/ExampleRenderExtension.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/Dx12Swapchain.h"
#include "dx12/GraphExecutor.h"
#include "dx12/PassParameters.h"
#include "dx12/ResourcePool.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// 使用显式异常，让 Release 和 Debug 的集成断言具有一致结果。
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 真实交换链需要 HWND，但整个验证不显示或激活用户可见窗口。
class HiddenWindow {
public:
    HiddenWindow() {
        WNDCLASSEXW description{sizeof(WNDCLASSEXW)};
        description.lpfnWndProc = DefWindowProcW;
        description.hInstance = GetModuleHandleW(nullptr);
        // 测试窗口按引擎命名并与正式宿主隔离，注册与注销必须使用同一标识。
        description.lpszClassName = L"EncoreRenderExtensionWindow";
        Require(RegisterClassExW(&description) != 0, "Register extension test window class");
        handle_ = CreateWindowExW(0, description.lpszClassName, L"Render extension test",
            WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, description.hInstance, nullptr);
        Require(handle_ != nullptr, "Create hidden extension test window");
    }
    ~HiddenWindow() {
        if (handle_) DestroyWindow(handle_);
        UnregisterClassW(L"EncoreRenderExtensionWindow", GetModuleHandleW(nullptr));
    }
    HWND Handle() const { return handle_; }

private:
    HWND handle_{}; // 晚于交换链和设备析构的隐藏原生窗口。
};

// 覆盖层显式依赖全部已注册 Pass，验证扩展不会绕过最终显示边界。
void Draw(render::FrameRenderer& renderer, gameplay::World& world, render::RenderSettings& settings,
    render::dx12::Dx12Device& device, render::dx12::DescriptorArena& descriptors,
    render::dx12::Dx12Swapchain& swapchain) {
    auto& backbuffer = swapchain.Current();
    renderer.Draw(world.ExtractRenderSnapshot(), settings, 1.0f / 60, backbuffer,
        [&](render::RenderGraph& graph) {
            render::dx12::PassParameters parameters;
            parameters.Access(renderer.Display(), render::ResourceAccess::Read, render::dx12::ShaderRead);
            parameters.Access(backbuffer, render::ResourceAccess::Write, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto overlay = parameters.AddPass(graph, "ExtensionTest.FinalOverlay",
                [&](const render::dx12::PassParameters&) {
                    const float color[]{0.04f, 0.08f, 0.12f, 1};
                    device.Commands()->ClearRenderTargetView(descriptors.Rtv(swapchain.CurrentIndex()), color, 0, nullptr);
                });
            // 覆盖层必须最后执行，但不能通过显式边把外部模块的无用工作保活。
            for (render::PassId before = 0; before < overlay; ++before) graph.AddOrdering(before, overlay);
        });
    Require(!renderer.PassNames().empty() && renderer.PassNames().back() == "ExtensionTest.FinalOverlay",
        "Final overlay must remain after all external module passes");
    swapchain.Present(false);
}

// Pass 名称来自真实编译计划，不能仅通过模块注册标记冒充 GPU 扩展执行。
void CheckExtensionPasses(const render::FrameRenderer& renderer, bool expected) {
    const auto& names = renderer.PassNames();
    const auto parent = std::find(names.begin(), names.end(), "PostProcess/Example.Copy");
    const auto child = std::find(names.begin(), names.end(), "PostProcess/Example.Copy/Readback");
    const auto parentUpload = std::find(names.begin(), names.end(), "Upload.PostProcess/Example.Copy");
    const auto childUpload = std::find(names.begin(), names.end(), "Upload.PostProcess/Example.Copy/Readback");
    Require((parent != names.end()) == expected && (child != names.end()) == expected,
        "Parent and subpass registration must both follow module lifetime");
    Require((parentUpload != names.end()) == expected && (childUpload != names.end()) == expected,
        "Each external pass must retain its own parameter upload");
    if (expected) {
        Require(parentUpload < parent && childUpload < child && parent < child,
            "RDG dependencies must preserve both uploads and parent-to-child copy order");
        const auto post = std::find(names.begin(), names.end(), "Post.VolumeBloomACES");
        Require(post != names.end() && post < parent, "PostProcess after-extension must read completed display output");
    }
}

// 即使另一张图具有数值相同的资源编号，也不能使用扩展上一张图的资源身份。
void CheckForeignGraphRejected(render::tests::ExampleRenderExtension& extension) {
    render::RenderGraph foreign;
    Require(extension.Output().graphId != render::InvalidGraphIndex && extension.Output().graphId < 10000,
        "The external resource must have a valid frame graph identity before testing collisions");
    for (render::ResourceId id = 0; id <= extension.Output().graphId; ++id)
        foreign.Import("Foreign." + std::to_string(id), 0);
    render::dx12::PassParameters parameters;
    parameters.Access(extension.Output(), render::ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bool rejected = false;
    try {
        parameters.AddPass(foreign, "Invalid.CrossGraphExtension", [](const render::dx12::PassParameters&) {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(rejected, "External module resources must reject another graph with colliding numeric IDs");
}
}

int main() {
    try {
        // 宿主拥有设备和帧，独立模块只通过公共管线注册接口参与渲染。
        HiddenWindow window;
        render::dx12::Dx12Device device(true);
        render::dx12::DescriptorArena descriptors(device);
        render::dx12::ResourcePool resources(device, descriptors);
        render::dx12::Dx12Swapchain swapchain(device, descriptors, window.Handle());
        constexpr unsigned width = 160, height = 120;
        swapchain.Resize(width, height);
        render::FrameRenderer renderer(device, descriptors, resources, width, height);
        gameplay::World world;
        world.CreateCornell(renderer.ReferenceScene());
        render::RenderSettings settings;
        settings.pipeline = 0;
        settings.shadows = false;
        settings.temporalReuse = false;
        settings.spatialReuse = false;
        render::tests::ExampleRenderExtension extension(device);
        render::IRenderExtension& externalModule = extension;
        externalModule.RegisterPasses(renderer.Pipeline());

        Draw(renderer, world, settings, device, descriptors, swapchain);
        CheckExtensionPasses(renderer, true);
        Require(extension.Readback() == render::tests::ExampleRenderExtension::ExpectedValues(width, height),
            "GPU readback must contain both external passes' independently uploaded values");
        CheckForeignGraphRejected(extension);

        // 第二帧复用模块自有存储，验证重新导入和状态跟踪覆盖连续帧。
        Draw(renderer, world, settings, device, descriptors, swapchain);
        CheckExtensionPasses(renderer, true);
        Require(extension.Readback() == render::tests::ExampleRenderExtension::ExpectedValues(width, height),
            "External resource reuse must retain correct values across frames");

        // 显式注销递归移除父子节点，下一帧必须同时失去 Pass 与参数上传。
        extension.UnregisterPasses(renderer.Pipeline());
        Draw(renderer, world, settings, device, descriptors, swapchain);
        CheckExtensionPasses(renderer, false);
        device.WaitIdle();
        std::cout << "External render module GPU integration passed: " << device.AdapterName() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
