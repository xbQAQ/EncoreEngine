#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <windows.h>

namespace render {
class RenderPipeline;
struct LaunchOptions {
    unsigned width{640}; // 内部渲染宽度，编辑器缩放不改变采样分辨率。
    unsigned height{480}; // 内部渲染高度，限制在设备支持的有效范围内。
    unsigned frames{}; // 为零时交互运行，正数用于固定帧数捕获。
    unsigned pipeline{}; // 编辑器默认光栅为 0，光追管线为 1。
    unsigned gi{1}; // 路径追踪为 0，屏幕空间 GI 为 1，ReSTIR GI 为 2。
    unsigned backend{}; // 软件 BVH 为 0，DXR 内联射线求交为 1。
    unsigned samples{1}; // 路径追踪器每帧独立相机路径数。
    unsigned shadowMode{2}; // 射线为 0，PCF 为 1，PCSS 为 2，VSM 为 3。
    bool headless{}; // 批处理 GPU 验证时隐藏宿主窗口。
    bool validation{}; // 启用 D3D12 调试消息与 GPU 错误报告。
    std::wstring capture; // 不含编辑器的最终渲染 PNG 输出路径。
    std::wstring editorCapture; // 包含编辑器叠加层的最终画面 PNG 输出路径。
    std::wstring scenePath; // 可选模型或 Protobuf 世界文档路径，空值加载默认世界。
};

// 编辑器宿主通过 PImpl 组合运行时模块，隐藏原生设备类型。
class Dx12Renderer {
public:
    Dx12Renderer(HWND window, const LaunchOptions& options);
    ~Dx12Renderer();
    void Frame();
    void Capture(const std::wstring& path);
    void CaptureEditor(const std::wstring& path);
    void Resize(unsigned width, unsigned height);
    // 导入校验在替换当前世界及其 GPU 场景前完成。
    void ImportScene(const std::wstring& path);
    // 模块在公共阶段注册 Pass/Subpass，注册表在场景切换时保持稳定。
    RenderPipeline& Pipeline();
private:
    class Impl;
    std::unique_ptr<Impl> impl_; // 隔离可复用接口与 Windows GPU 宿主状态。
};
}
