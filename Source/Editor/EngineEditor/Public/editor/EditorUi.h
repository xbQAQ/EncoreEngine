#pragma once

#include "gameplay/Components.h"
#include "editor/EditorGizmo.h"
#include "editor/EditorHistory.h"
#include "editor/EditorNavigation.h"
#include "editor/EditorObjectPalette.h"
#include "editor/EditorScriptUi.h"
#include "editor/EditorContentBrowser.h"
#include "editor/EditorDefaults.h"
#include "editor/EditorProfilerUi.h"
#include "editor/EditorWorldUi.h"
#include <windows.h>
#include <optional>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList;
struct ImGuiContext;

namespace gameplay { class World; }
namespace render { struct RenderSettings; }
namespace render { class RenderGraph; }
namespace render::dx12 { class Dx12Device; class DescriptorArena; struct GpuResource; }

namespace render::editor {
enum class GizmoOperation {
    Select, // 只选择可见实体，不显示变换手柄，对应 Q 工具。
    Translate, // 沿世界或局部坐标轴移动所选位姿。
    Rotate, // 绕所选对象的可见中心旋转。
    Scale, // 沿网格局部轴缩放，避免引入剪切。
};

struct GizmoInteraction {
    GizmoOperation operation{GizmoOperation::Translate}; // 工具栏与快捷键共享的活动变换工具。
    bool local{}; // 启用局部轴；关闭时移动和旋转使用世界轴。
    bool snap{}; // 每次操作应用当前工具的吸附步长。
    float translationStep{0.1f}; // 以米为单位的移动吸附步长。
    float rotationStep{15}; // 以度为单位的旋转吸附步长。
    float scaleStep{0.1f}; // 局部缩放的乘法吸附增量。
    gameplay::Entity target{entt::null}; // 当前交互的所属实体，每帧重新检查有效性。
    gameplay::Transform start; // Escape 取消操作时恢复的初始位姿。
    Vec3 pivot; // 所选网格不可变源包围盒中心的缓存。
    bool pivotCached{}; // 防止每个交互帧重新扫描大型导入网格。
    bool dragging{}; // 仅在 ImGuizmo 持有变换拖动时为真。
    bool cancelled{}; // 取消后等待左键释放，防止同一次按压重新激活。
};

// 编辑器拥有界面状态和 ImGui 后端，GPU 命令仍由调用方的 RDG 阶段记录。
class EditorUi {
public:
    EditorUi(HWND window, dx12::Dx12Device& device, dx12::DescriptorArena& descriptors, bool persistLayout = true);
    ~EditorUi();
    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    void BeginFrame();
    void Build(RenderSettings& settings, gameplay::World& world, const Scene& source, unsigned clientWidth, unsigned clientHeight,
        unsigned renderWidth, unsigned renderHeight, unsigned accumulated, float delta,
        const std::vector<std::string>& passes);
    void EndFrame();
    void Draw(ID3D12GraphicsCommandList* commands) const;
    bool UpdatePlatformWindows();
    void DrawPlatformWindows() const;
    void CreateGpuObjects();
    std::wstring ConsumeImportRequest();
    std::optional<WorldUiRequest> ConsumeWorldRequest();
    void SetImportError(std::string error);
    void ClearSelection();
    EditorScriptUi& Scripts() { return scripts_; }
    void SetProfile(const GraphProfile& profile) { profiler_.Update(profile); }
    // 录制请求独立于场景作者设置，不进入撤销或世界序列化。
    bool ProfileRecording() const { return profiler_.Recording(); }
    void StopProfileRecording() { profiler_.StopRecording(); }
    void AddOverlayPass(RenderGraph& graph, dx12::GpuResource& backbuffer,
        dx12::GpuResource& display, unsigned backbufferIndex);

private:
    void SettingsPanel(RenderSettings& settings, unsigned accumulated, float delta,
        const std::vector<std::string>& passes);
    void WorldPanel(RenderSettings& settings, gameplay::World& world);
    void InspectEntity(RenderSettings& settings, gameplay::World& world);
    void DetailsPanel(RenderSettings& settings, gameplay::World& world);
    void RequestImport();
    void BuildDockspace(unsigned clientWidth, unsigned clientHeight);
    void Viewport(RenderSettings& settings, gameplay::World& world, const Scene& source, unsigned renderWidth, unsigned renderHeight,
        float delta);
    void GizmoToolbar(const gameplay::World& world);
    void CreatePlacedObject(RenderSettings& settings, gameplay::World& world, std::uint32_t preset,
        const gameplay::Transform& transform);
    void CancelGizmo();
    void EditorShortcuts(RenderSettings& settings, gameplay::World& world);
    void EditMenu(RenderSettings& settings, gameplay::World& world);
    void BeginPropertyFrame(const RenderSettings& settings, const gameplay::World& world);
    void EndPropertyFrame(const RenderSettings& settings, const gameplay::World& world);
    void BeginGizmoTransaction(const RenderSettings& settings, const gameplay::World& world);
    void CommitGizmoTransaction(const RenderSettings& settings, const gameplay::World& world);
    void ReplayHistory(RenderSettings& settings, gameplay::World& world, bool redo);
    void DeleteSelection(RenderSettings& settings, gameplay::World& world);
    void DuplicateSelection(RenderSettings& settings, gameplay::World& world);
    void NavigateViewport(RenderSettings& settings, gameplay::World& world, const Scene& source,
        const ViewportRect& image, float aspect, float delta, bool imageHovered, bool focused, bool captured);

    HWND window_{}; // 模态导入对话框借用的原生父窗口，寿命长于编辑器。
    dx12::Dx12Device& device_; // 借用设备元数据和图形后端，寿命长于编辑器。
    dx12::DescriptorArena& descriptors_; // 借用可见描述符堆，保留字体槽 0 与视口槽 1。
    ImGuiContext* context_{}; // 编辑器独占并销毁的 ImGui 上下文。
    std::string layoutFilename_; // 用户本地应用目录中的布局文件；自动化验证保持为空。
    bool resetLayout_{}; // 菜单请求在下一帧提交面板前重建默认停靠树。
    gameplay::Entity selection_{entt::null}; // 带代际的选择句柄，访问组件前重新校验。
    std::wstring pendingImport_; // 界面帧结束后由宿主消费一次的模型导入路径。
    std::string importError_; // 保留最近一次导入错误，直到新导入开始或成功。
    int newLightType_{2}; // 创建菜单所选光源形状，与 gameplay::LightType 对齐。
    bool gpuObjectsCreated_{}; // 防止后端延迟上传逃出 RDG 字体上传阶段。
    bool cameraDragging_{}; // 保持当前导航手势，直到所属鼠标键释放。
    GizmoInteraction gizmo_; // 与渲染资源独立的变换工具配置和拖动所有权。
    EditorHistory history_; // 只记录作者编辑，导航和模拟不创建事务。
    EditorScriptUi scripts_; // 源码编辑及运行时诊断，不拥有渲染资源。
    EditorContentBrowser content_; // 资源与本地文件导航，只借用当前选择。
    EditorObjectPalette objectPalette_; // 常用预设搜索、原生拖动及创建失败的持久反馈。
    SurfacePickCache surfacePicker_; // 点击选择、材质投放和物体放置共用，导航不重建世界空间场景。
    EditorDefaults defaults_; // 编辑期间保持稳定的导入或创建基线。
    EditorProfilerUi profiler_; // 显式录制并回看已完成的 CPU 与 GPU 计时样本。
    EditorWorldUi worldUi_; // 将世界文档操作作为延迟请求交给宿主。
    bool defaultsReady_{}; // 等待替换后的 World 可用时再捕获基线。
    std::optional<EditorDocument> propertyBefore_; // 当前面板控件修改前的逐帧作者快照。
    std::optional<EditorDocument> gizmoBefore_; // 完整变换操作开始时仅保存目标位姿。
    unsigned propertyFrameOwner_{}; // 当前捕获属性的根面板标识。
    unsigned propertyOwner_{}; // 最近一次属性事务所属面板，防止跨面板合并。
    unsigned propertyActiveId_{}; // 所属面板上一次活动控件，用于连续操作合并。
    bool propertyRecorded_{}; // 当前属性交互是否已拥有历史记录。
    EditorNavigation navigation_; // 不依赖渲染器的相机速度与环绕支点。
    NavigationMode navigationMode_{NavigationMode::None}; // 导航捕获模式，离开图像后仍保持。
    int navigationButton_{-1}; // 持有导航的鼠标键，直到释放或失焦。
};
}
