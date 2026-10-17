#include "editor/EditorUi.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace render::editor {
void EditorUi::BeginPropertyFrame(const RenderSettings& settings, const gameplay::World& world) {
    // 只捕获当前面板的作者修改；视口导航不能进入属性事务。
    propertyFrameOwner_ = ImGui::GetCurrentWindow()->RootWindowPopupTree->ID;
    propertyBefore_ = CaptureEditorDocument(world, settings, selection_);
}

void EditorUi::EndPropertyFrame(const RenderSettings& settings, const gameplay::World& world) {
    const auto& context = *ImGui::GetCurrentContext();
    const auto active = context.ActiveIdWindow && context.ActiveIdWindow->RootWindowPopupTree->ID == propertyFrameOwner_ ? context.ActiveId : 0u;
    // 颜色选择等所属弹窗共用面板边界；其他面板的空捕获不能打断或借用活动交互。
    const bool owner = propertyOwner_ == propertyFrameOwner_;
    const bool continuing = owner && propertyActiveId_ != 0 && (active == propertyActiveId_ || active == 0);
    if (propertyBefore_ && history_.Record(*propertyBefore_, CaptureEditorDocument(world, settings, selection_),
        "编辑属性", continuing && propertyRecorded_)) {
        propertyOwner_ = propertyFrameOwner_;
        propertyActiveId_ = active;
        propertyRecorded_ = active != 0;
    } else if (owner) {
        if (!continuing || active == 0) propertyRecorded_ = false;
        propertyActiveId_ = active;
    }
    propertyBefore_.reset();
}

void EditorUi::BeginGizmoTransaction(const RenderSettings& settings, const gameplay::World& world) {
    if (gizmoBefore_ || !world.Registry().valid(selection_)) return;
    // 只保存目标的作者位姿；其他实体可在鼠标帧之间继续模拟。
    EditorDocument before;
    before.settings = settings;
    before.selection = selection_;
    EditorEntitySnapshot target;
    target.entity = selection_;
    target.transform = world.Registry().get<gameplay::Transform>(selection_);
    before.entities.push_back(std::move(target));
    gizmoBefore_ = std::move(before);
}

void EditorUi::CommitGizmoTransaction(const RenderSettings& settings, const gameplay::World& world) {
    // 多帧拖动形成一笔原子历史，单击或取消不创建空命令。
    if (gizmoBefore_ && world.Registry().valid(gizmoBefore_->entities.front().entity)) {
        auto after = *gizmoBefore_;
        after.selection = selection_;
        after.entities.front().transform = world.Registry().get<gameplay::Transform>(after.entities.front().entity);
        // 即使手势中其他控件改变了设置，两端设置仍保持相同。
        gizmoBefore_->settings = after.settings = settings;
        history_.Record(*gizmoBefore_, after, "变换");
    }
    gizmoBefore_.reset();
    propertyActiveId_ = 0;
    propertyRecorded_ = false;
}

void EditorUi::ReplayHistory(RenderSettings& settings, gameplay::World& world, bool redo) {
    if (gizmo_.dragging) {
        // 操作中撤销先取消当前手势，不接触已完成的较早事务。
        if (!redo && world.Registry().valid(gizmo_.target)) {
            world.Registry().get<gameplay::Transform>(gizmo_.target) = gizmo_.start;
            world.MarkDirty();
            settings.reset = true;
            gizmoBefore_.reset();
            CancelGizmo();
        }
        return;
    }
    if (redo) history_.Redo(world, settings, selection_);
    else history_.Undo(world, settings, selection_);
    // 回放使缓存包围盒与捕获失效，回放本身不进入历史。
    CancelGizmo();
    navigationMode_ = NavigationMode::None;
    navigationButton_ = -1;
    cameraDragging_ = false;
    propertyActiveId_ = 0;
    propertyRecorded_ = false;
}

void EditorUi::EditorShortcuts(RenderSettings& settings, gameplay::World& world) {
    const auto& io = ImGui::GetIO();
    // 文本和数值输入保留 ImGui 局部撤销；全局命令要求编辑器焦点。
    if (io.AppFocusLost || io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) ||
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) ||
        (ImGui::IsAnyItemActive() && !gizmo_.dragging) || cameraDragging_) return;
    if (io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) ReplayHistory(settings, world, io.KeyShift);
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) ReplayHistory(settings, world, true);
        else if (!gizmo_.dragging && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelection(settings, world);
    }
    else if (!io.KeyAlt && !gizmo_.dragging && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelection(settings, world);
}

void EditorUi::EditMenu(RenderSettings& settings, gameplay::World& world) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("编辑###Edit")) {
        // 事务标签只影响显示，历史回放与编辑器语言无关。
        const auto undo = std::string("撤销 ") + std::string(history_.UndoLabel());
        const auto redo = std::string("重做 ") + std::string(history_.RedoLabel());
        // 菜单操作先于属性捕获，保证撤销不记录新的作者命令。
        if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, history_.CanUndo() && !gizmo_.dragging)) ReplayHistory(settings, world, false);
        if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, history_.CanRedo() && !gizmo_.dragging)) ReplayHistory(settings, world, true);
        ImGui::EndMenu();
    }
    // 布局属于编辑器个人配置；恢复请求延迟到下一帧，避免在遍历面板时拆除停靠树。
    if (ImGui::BeginMenu("布局###Layout")) {
        if (ImGui::MenuItem("恢复默认布局###Reset Layout")) resetLayout_ = true;
        if (ImGui::MenuItem("保存布局###Save Layout", nullptr, false, !layoutFilename_.empty()))
            ImGui::SaveIniSettingsToDisk(layoutFilename_.c_str());
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void EditorUi::DeleteSelection(RenderSettings& settings, gameplay::World& world) {
    auto& registry = world.Registry();
    if (!registry.valid(selection_) || selection_ == world.CameraEntity() || registry.all_of<gameplay::Camera>(selection_)) return;
    const auto before = CaptureEditorDocument(world, settings, selection_);
    // 发光体代理选择时已解析到光源拥有者，删除光源也移除其提取几何。
    world.DestroyEntity(selection_);
    selection_ = entt::null;
    settings.reset = true;
    CancelGizmo();
    history_.Record(before, CaptureEditorDocument(world, settings, selection_), "删除实体");
}

void EditorUi::DuplicateSelection(RenderSettings& settings, gameplay::World& world) {
    auto& registry = world.Registry();
    if (!registry.valid(selection_) || registry.all_of<gameplay::Camera>(selection_)) return;
    unsigned activeLights = 0;
    for (const auto entity : registry.view<gameplay::Light>()) activeLights += registry.get<gameplay::Light>(entity).enabled ? 1u : 0u;
    const auto* sourceLight = registry.try_get<gameplay::Light>(selection_);
    if (sourceLight && sourceLight->enabled && activeLights >= 16) return;
    const auto before = CaptureEditorDocument(world, settings, selection_);
    const auto original = selection_;
    const auto* name = registry.try_get<gameplay::Name>(original);
    const auto duplicate = world.CreateEntity(name ? name->value + " 副本" : "实体副本");
    registry.get<gameplay::Transform>(duplicate) = registry.get<gameplay::Transform>(original);
    // 网格副本共享不可变资源；光源副本拥有新生成的发光几何。
    if (const auto* mesh = registry.try_get<gameplay::MeshRenderer>(original)) registry.emplace<gameplay::MeshRenderer>(duplicate, *mesh);
    if (const auto* light = registry.try_get<gameplay::Light>(original)) {
        auto copy = *light;
        copy.emitterMaterial = ~0u;
        registry.emplace<gameplay::Light>(duplicate, copy);
    }
    if (const auto* velocity = registry.try_get<gameplay::Velocity>(original)) registry.emplace<gameplay::Velocity>(duplicate, *velocity);
    if (const auto* script = registry.try_get<gameplay::LuaScript>(original)) registry.emplace<gameplay::LuaScript>(duplicate, *script);
    selection_ = duplicate;
    world.MarkDirty();
    settings.reset = true;
    CancelGizmo();
    history_.Record(before, CaptureEditorDocument(world, settings, selection_), "复制实体");
}
}
