#include "render/Dx12Renderer.h"
#include "render/RenderSettings.h"
#include "gameplay/World.h"
#include "editor/EditorUi.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/Dx12Swapchain.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"
// 编辑器宿主在组合边界持有帧渲染器，算法实现保持在运行时模块。
#include "Frame/FrameRenderer.h"
#include "Capture/CaptureService.h"
#include "asset/GltfImporter.h"
#include "asset/SceneImporter.h"
#include "render/SceneExtraction.h"
#include "render/WorldDocument.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <cwctype>

namespace render {
// 外观类只组合模块寿命，GPU 分配与渲染算法由各自模块实现。
class Dx12Renderer::Impl {
public:
    Impl(HWND window, const LaunchOptions& options)
        : window_(window), options_(options), device_(options.validation), descriptors_(device_),
          resources_(device_, descriptors_), swapchain_(device_, descriptors_, window),
          editor_(window, device_, descriptors_, !options.headless) {
        // 离屏验证使用独立内存布局，不读取或覆盖交互编辑器的个人分隔比例。
        settings_.pipeline = options.pipeline;
        settings_.gi = options.gi;
        settings_.backend = options.backend;
        settings_.samples = options.samples;
        settings_.shadowMode = options.shadowMode;
        if (settings_.pipeline == 1 && settings_.backend == 1 && !device_.DxrSupported())
            throw std::runtime_error("DXR 1.1 is unavailable");
        // 默认世界完全来自 Protobuf；glTF 仍可作为单场景世界导入。
        const std::filesystem::path path(options.scenePath);
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (path.empty() || extension == L".textproto" || extension == L".pb" || extension == L".world") {
            gameplay::World candidate;
            candidate.CreateDocument(LoadWorldDocument(path.empty() ? DefaultWorldPath() : path));
            SetWorld(std::move(candidate));
        } else SetScene(ImportSceneFile(path), false);
        device_.BeginCommands();
        RenderGraph graph;
        graph.AddPass("Editor.UploadFonts", {}, [this] { editor_.CreateGpuObjects(); }, true);
        dx12::GraphExecutor executor(device_);
        executor.Execute(graph);
        device_.SubmitAndWait();
        std::cout << "Adapter: " << device_.AdapterName() << "; DXR1.1: " << (device_.DxrSupported() ? "yes" : "no") << '\n';
    }

    ~Impl() noexcept {
        // 先于编辑器和渲染器成员析构等待主队列；沿用设备的有限超时，异常退出也不无限重试。
        try { device_.WaitIdle(); }
        catch (const std::exception& error) { std::cerr << "Renderer retirement: " << error.what() << '\n'; }
        catch (...) { std::cerr << "Renderer retirement: unknown GPU cleanup failure\n"; }
    }

    void SetScene(Scene source, bool cornell) {
        gameplay::World candidate;
        candidate.CreateScene(source, cornell);
        SetWorld(std::move(candidate));
    }

    gameplay::World PrepareModelImport(const std::filesystem::path& path) const {
        // 先解析完整文件，再在候选世界中追加；保留当前场景对象、相机和已编辑参数。
        auto imported = ImportSceneFile(path);
        gameplay::World candidate;
        candidate.CreateDocument(world_.ExportDocument(), world_.ActiveSceneId());
        candidate.AppendScene(imported);
        return candidate;
    }

    void SetWorld(gameplay::World candidate) {
        // 通用创建模板来自正式配置，进入宿主前安装一次源几何；拖放只创建共享资源的实体实例。
        candidate.InstallObjectPresets(LoadWorldDocument(DefaultWorldPath().parent_path().parent_path() / L"Objects/basic.textproto"));
        // 作者数据始终归属 World，渲染器只接收当前场景的扁平传输副本。
        auto source = candidate.CookResourceScene();
        // 在销毁 GPU 场景前完成 CPU 校验，错误文档不影响当前世界。
        (void)ExtractRenderScene(source, candidate.ExtractRenderSnapshot(), source.cornell);
        device_.WaitIdle();
        capture_.reset();
        renderer_.reset();
        resources_.Reset();
        descriptors_.ResetPersistent();
        renderer_ = std::make_unique<FrameRenderer>(device_, descriptors_, resources_, options_.width, options_.height, std::move(source));
        // 扩展注册属于宿主寿命，世界替换只重新接入同一个管线。
        renderer_->UsePipeline(pipeline_);
        // 世界替换会重置采集序号；结束旧录制但保留其结果，避免混入另一世界的数据。
        editor_.StopProfileRecording();
        capture_ = std::make_unique<CaptureService>(device_, resources_);
        world_ = std::move(candidate);
        editor_.ClearSelection();
        // 新世界必须重新提交脚本，即使为空也要清除前一世界的材质程序。
        editor_.Scripts().RequestMaterialCompile();
        if (options_.frames) {
            // 固定帧数捕获必须在第一帧采用文档脚本，不能因后台编译尚未完成而截取旧材质。
            const auto scripts = world_.MaterialScripts();
            if (!scripts.empty()) {
                (void)editor_.Scripts().ConsumeMaterialCompileRequest();
                editor_.Scripts().SetMaterialCompileResult(renderer_->ReloadMaterials(scripts));
            }
        }
        lastPresented_ = nullptr;
        settings_.reset = true;
    }

    void Frame() {
        RECT client{};
        GetClientRect(window_, &client);
        swapchain_.Resize(std::max(1L, client.right), std::max(1L, client.bottom));
        const auto now = std::chrono::steady_clock::now();
        const float delta = std::clamp(std::chrono::duration<float>(now - previousTime_).count(), 0.001f, 0.1f);
        previousTime_ = now;
        // 先执行模拟，再绘制属性面板，使相机和对象数值在同一帧反映运行结果。
        auto& gameplayScripts = editor_.Scripts().Gameplay();
        gameplayScripts.SetPlaying(settings_.simulate);
        gameplayScripts.Tick(world_, delta);
        if (settings_.simulate) world_.Update(delta);
        editor_.SetProfile(renderer_->Profile());
        editor_.BeginFrame();
        // 视口拾取与变换中心使用同一份不可变源几何。
        editor_.Build(settings_, world_, renderer_->ReferenceScene(), swapchain_.Width(), swapchain_.Height(), options_.width, options_.height,
            renderer_->Accumulated(), delta, renderer_->PassNames());
        editor_.EndFrame();
        const auto request = editor_.ConsumeImportRequest();
        if (!request.empty()) {
            // 解析错误保留当前世界；设备分配失败交给上层报告原始诊断。
            std::optional<gameplay::World> candidate;
            try {
                candidate.emplace(PrepareModelImport(std::filesystem::path(request)));
            } catch (const std::exception& error) { editor_.SetImportError(error.what()); }
            if (candidate) { SetWorld(std::move(*candidate)); editor_.SetImportError({}); }
        }
        if (const auto operation = editor_.ConsumeWorldRequest()) {
            // 所有文档操作延迟到 ImGui 帧结束，避免面板遍历期间更换注册表。
            std::optional<gameplay::World> replacement;
            try {
                if (operation->kind == editor::WorldUiRequestKind::Save)
                    SaveWorldDocument(world_.ExportDocument(), std::filesystem::path(operation->path));
                else {
                    gameplay::World candidate;
                    if (operation->kind == editor::WorldUiRequestKind::Open)
                        candidate.CreateDocument(LoadWorldDocument(std::filesystem::path(operation->path)));
                    else candidate.CreateDocument(world_.ExportDocument(), operation->sceneId);
                    replacement.emplace(std::move(candidate));
                }
                editor_.SetImportError({});
            } catch (const std::exception& error) { editor_.SetImportError(error.what()); }
            // GPU 替换失败保留原始设备异常，不能在资源池已重置后继续绘制旧场景。
            if (replacement) SetWorld(std::move(*replacement));
        }
        // 先消费最新源再发布完成结果，过期编译候选不能覆盖新文档。
        if (editor_.Scripts().ConsumeMaterialCompileRequest()) {
            renderer_->RequestMaterialReload(world_.MaterialScripts());
            editor_.Scripts().SetMaterialCompilePending();
        }
        if (auto result = renderer_->PollMaterialReload()) {
            if (result->success) settings_.reset = true;
            editor_.Scripts().SetMaterialCompileResult(std::move(*result));
        }
        const auto snapshot = world_.ExtractRenderSnapshot();
        auto& backbuffer = swapchain_.Current();
        // 界面开关在同一帧生效，未录制或达到帧上限时不创建任何性能查询命令。
        renderer_->SetProfilingEnabled(editor_.ProfileRecording());
        renderer_->Draw(snapshot, settings_, delta, backbuffer, [&](RenderGraph& graph) {
            editor_.AddOverlayPass(graph, backbuffer, renderer_->Display(), swapchain_.CurrentIndex());
        });
        // 主图已提交并等待，平台窗口才可跨独立队列读取共享 Display 和描述符。
        if (editor_.UpdatePlatformWindows()) {
            device_.BeginCommands();
            RenderGraph platformGraph;
            dx12::GraphExecutor executor(device_);
            executor.Import(platformGraph, renderer_->Display());
            platformGraph.AddPass("Editor.PlatformWindows", {dx12::Read(renderer_->Display())},
                [this] { editor_.DrawPlatformWindows(); }, true);
            // ImGui 后端自持平台交换链及队列围栏；其呈现等待构成显式外部边界，下一帧才可重用 Display。
            platformGraph.Export(renderer_->Display().graphId, dx12::ShaderRead);
            executor.Execute(platformGraph);
            device_.SubmitAndWait();
        }
        lastPresented_ = &backbuffer;
        swapchain_.Present(!options_.frames);
    }

    void Capture(const std::wstring& path) { capture_->Capture(renderer_->Display(), path); }
    void CaptureEditor(const std::wstring& path) {
        if (!lastPresented_) throw std::logic_error("Render a frame before capturing the editor");
        capture_->Capture(*lastPresented_, path);
    }
    void Resize(unsigned width, unsigned height) { swapchain_.Resize(width, height); }
    RenderPipeline& Pipeline() { return pipeline_; }
    void ImportScene(const std::wstring& path) {
        SetWorld(PrepareModelImport(std::filesystem::path(path)));
        editor_.ClearSelection();
        editor_.SetImportError({});
    }
private:
    HWND window_{}; // 借用宿主窗口以读取输入和呈现尺寸。
    LaunchOptions options_; // 命令行渲染尺寸与批处理策略。
    RenderSettings settings_; // 独立于渲染器的可编辑设置。
    gameplay::World world_; // 当前场景实体、组件和资源的所有者。
    dx12::Dx12Device device_; // 设备、队列与提交同步的所有者。
    dx12::DescriptorArena descriptors_; // GPU 与 CPU 描述符的分配所有者。
    dx12::ResourcePool resources_; // GPU 资源及上传内存的分配所有者。
    dx12::Dx12Swapchain swapchain_; // 呈现表面与后备缓冲的所有者。
    RenderPipeline pipeline_; // 外部模块注册的稳定管线，不随世界替换而销毁。
    std::unique_ptr<FrameRenderer> renderer_; // 可替换场景渲染器及其历史图像。
    editor::EditorUi editor_; // 编辑器状态与最终叠加 Pass。
    std::unique_ptr<CaptureService> capture_; // 图资源读回与 PNG 输出服务。
    dx12::GpuResource* lastPresented_{}; // 借用上一帧后备缓冲以捕获编辑器画面。
    std::chrono::steady_clock::time_point previousTime_{std::chrono::steady_clock::now()}; // 帧间隔计算起点。
};

Dx12Renderer::Dx12Renderer(HWND window, const LaunchOptions& options) : impl_(std::make_unique<Impl>(window, options)) {}
Dx12Renderer::~Dx12Renderer() = default;
void Dx12Renderer::Frame() { impl_->Frame(); }
void Dx12Renderer::Capture(const std::wstring& path) { impl_->Capture(path); }
void Dx12Renderer::CaptureEditor(const std::wstring& path) { impl_->CaptureEditor(path); }
void Dx12Renderer::Resize(unsigned width, unsigned height) { impl_->Resize(width, height); }
RenderPipeline& Dx12Renderer::Pipeline() { return impl_->Pipeline(); }
void Dx12Renderer::ImportScene(const std::wstring& path) { impl_->ImportScene(path); }
}
