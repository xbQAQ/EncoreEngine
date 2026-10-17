#include "editor/EditorUi.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include "dx12/DescriptorArena.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace render::editor {
namespace {
gameplay::RenderCamera ViewCamera(const gameplay::World& world) {
    gameplay::RenderCamera result{entt::null, {{0, 1.4f, -4.6f}, {}, {1, 1, 1}}, {}};
    // 活动相机的确定性排序及无相机回退与渲染侧 ViewState 保持一致。
    for (auto [entity, transform, camera] : world.Registry().view<const gameplay::Transform, const gameplay::Camera>().each())
        if (camera.active && (result.entity == entt::null || entt::to_integral(entity) < entt::to_integral(result.entity)))
            result = {entity, transform, camera};
    return result;
}

bool ToolButton(const char* id, const char* tooltip, GizmoOperation operation, bool selected, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(id, ImVec2(27, 25));
    if (selected) ImGui::PopStyleColor();
    const auto corner = ImGui::GetItemRectMin();
    const ImVec2 center(corner.x + 13.5f, corner.y + 12.5f);
    const auto color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    auto* draw = ImGui::GetWindowDrawList();
    // 使用熟悉的变换符号，并保留紧凑、稳定的点击范围。
    if (operation == GizmoOperation::Select) {
        draw->AddTriangleFilled(ImVec2(center.x - 4, center.y - 8), ImVec2(center.x - 4, center.y + 7),
            ImVec2(center.x + 6, center.y + 2), color);
    } else if (operation == GizmoOperation::Translate) {
        draw->AddLine(ImVec2(center.x - 7, center.y), ImVec2(center.x + 7, center.y), color, 1.5f);
        draw->AddLine(ImVec2(center.x, center.y - 7), ImVec2(center.x, center.y + 7), color, 1.5f);
        draw->AddTriangleFilled(ImVec2(center.x - 8, center.y), ImVec2(center.x - 4, center.y - 3), ImVec2(center.x - 4, center.y + 3), color);
        draw->AddTriangleFilled(ImVec2(center.x + 8, center.y), ImVec2(center.x + 4, center.y - 3), ImVec2(center.x + 4, center.y + 3), color);
        draw->AddTriangleFilled(ImVec2(center.x, center.y - 8), ImVec2(center.x - 3, center.y - 4), ImVec2(center.x + 3, center.y - 4), color);
        draw->AddTriangleFilled(ImVec2(center.x, center.y + 8), ImVec2(center.x - 3, center.y + 4), ImVec2(center.x + 3, center.y + 4), color);
    } else if (operation == GizmoOperation::Rotate) {
        draw->PathArcTo(center, 7, -0.5f, 4.7f, 20);
        draw->PathStroke(color, 0, 1.8f);
        draw->AddTriangleFilled(ImVec2(center.x + 8, center.y - 5), ImVec2(center.x + 2, center.y - 5), ImVec2(center.x + 7, center.y + 1), color);
    } else {
        draw->AddRect(ImVec2(center.x - 7, center.y + 2), ImVec2(center.x - 2, center.y + 7), color, 0, 0, 1.3f);
        draw->AddLine(ImVec2(center.x - 1, center.y + 1), ImVec2(center.x + 7, center.y - 7), color, 1.8f);
        draw->AddLine(ImVec2(center.x + 2, center.y - 7), ImVec2(center.x + 7, center.y - 7), color, 1.8f);
        draw->AddLine(ImVec2(center.x + 7, center.y - 7), ImVec2(center.x + 7, center.y - 2), color, 1.8f);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void MarkManipulated(RenderSettings& settings, gameplay::World& world) {
    // 位姿编辑必须同时失效场景上传、BVH 与所有光传输历史。
    world.MarkDirty();
    settings.reset = true;
}

bool PoseChanged(const gameplay::Transform& a, const gameplay::Transform& b) {
    return a.position.x != b.position.x || a.position.y != b.position.y || a.position.z != b.position.z ||
        a.rotationDegrees.x != b.rotationDegrees.x || a.rotationDegrees.y != b.rotationDegrees.y || a.rotationDegrees.z != b.rotationDegrees.z ||
        a.scale.x != b.scale.x || a.scale.y != b.scale.y || a.scale.z != b.scale.z;
}
}

void EditorUi::CancelGizmo() {
    ImGui::SetCurrentContext(context_);
    ImGuizmo::SetImGuiContext(context_);
    // 固定标识让导入或删除可在视口外取消工具包的真实交互。
    ImGuizmo::PushID("EditorTransform");
    ImGuizmo::Enable(false);
    ImGuizmo::PopID();
    gizmo_.target = entt::null;
    gizmo_.dragging = false;
    gizmo_.pivotCached = false;
    gizmo_.cancelled = ImGui::IsMouseDown(ImGuiMouseButton_Left);
}

void EditorUi::GizmoToolbar(const gameplay::World& world) {
    const float width = ImGui::GetContentRegionAvail().x;
    const auto& registry = world.Registry();
    const bool light = registry.valid(selection_) && registry.all_of<gameplay::Light>(selection_);
    const bool enabled = !gizmo_.dragging && !cameraDragging_ && !objectPalette_.Dragging();
    // 光源尺寸保存在 Light 中，Transform 缩放手柄不会产生有效编辑。
    if (light && gizmo_.operation == GizmoOperation::Scale) gizmo_.operation = GizmoOperation::Translate;
    if (ToolButton("##Select", "选择", GizmoOperation::Select, gizmo_.operation == GizmoOperation::Select, enabled))
        gizmo_.operation = GizmoOperation::Select;
    if (width >= 62) ImGui::SameLine();
    if (ToolButton("##Translate", "移动", GizmoOperation::Translate, gizmo_.operation == GizmoOperation::Translate, enabled))
        gizmo_.operation = GizmoOperation::Translate;
    if (width >= 97) ImGui::SameLine();
    if (ToolButton("##Rotate", "旋转", GizmoOperation::Rotate, gizmo_.operation == GizmoOperation::Rotate, enabled))
        gizmo_.operation = GizmoOperation::Rotate;
    if (width >= 132) ImGui::SameLine();
    if (ToolButton("##Scale", light ? "光源尺寸" : "缩放", GizmoOperation::Scale,
        gizmo_.operation == GizmoOperation::Scale, enabled && !light)) gizmo_.operation = GizmoOperation::Scale;

    // 缩放显式使用局部坐标，世界轴非均匀缩放会引入 Transform 无法表达的剪切。
    if (width >= 300) ImGui::SameLine(0, 14);
    ImGui::BeginDisabled(!enabled || gizmo_.operation == GizmoOperation::Scale || gizmo_.operation == GizmoOperation::Select);
    const bool local = gizmo_.local || gizmo_.operation == GizmoOperation::Scale;
    // 本地化标题保留原标识，文本变化不会丢失交互状态。
    if (ImGui::RadioButton("局部###Local", local)) gizmo_.local = true;
    if (width >= 150) ImGui::SameLine();
    if (ImGui::RadioButton("世界###World", !local)) gizmo_.local = false;
    ImGui::EndDisabled();
    if (width >= 500) ImGui::SameLine(0, 14);
    ImGui::BeginDisabled(!enabled || gizmo_.operation == GizmoOperation::Select);
    ImGui::Checkbox("吸附###Snap", &gizmo_.snap);
    if (width >= 175) ImGui::SameLine();
    float& step = gizmo_.operation == GizmoOperation::Translate ? gizmo_.translationStep :
        gizmo_.operation == GizmoOperation::Rotate ? gizmo_.rotationStep : gizmo_.scaleStep;
    const char* suffix = gizmo_.operation == GizmoOperation::Translate ? "米" : gizmo_.operation == GizmoOperation::Rotate ? "度" : "倍";
    ImGui::SetNextItemWidth(std::max(20.0f, std::min(76.0f, width - 42)));
    float candidate = step;
    if (ImGui::DragFloat("##SnapStep", &candidate, gizmo_.operation == GizmoOperation::Rotate ? 1.0f : 0.01f,
        0.001f, 1000, "%.3f", ImGuiSliderFlags_AlwaysClamp) && std::isfinite(candidate)) step = candidate;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("吸附步长（%s）", suffix);
    if (width >= 130) ImGui::SameLine();
    ImGui::TextUnformatted(suffix);
    ImGui::EndDisabled();
    const auto* name = registry.valid(selection_) ? registry.try_get<gameplay::Name>(selection_) : nullptr;
    // 堆叠布局下选择名称自动换行，窗口缩窄后仍保持可读。
    if (name) ImGui::TextWrapped("%s", name->value.c_str());
    else ImGui::TextDisabled("未选择实体");
    ImGui::Separator();
}

void EditorUi::Viewport(RenderSettings& settings, gameplay::World& world, const Scene& source,
    unsigned renderWidth, unsigned renderHeight, float delta) {
    if (selection_ != gizmo_.target) {
        CommitGizmoTransaction(settings, world);
        CancelGizmo();
        gizmo_.target = selection_;
    }
    GizmoToolbar(world);
    const auto available = ImGui::GetContentRegionAvail();
    if (available.x <= 0 || available.y <= 0 || renderWidth == 0 || renderHeight == 0) {
        CancelGizmo();
        cameraDragging_ = false;
        return;
    }
    const float fit = std::min(available.x / static_cast<float>(renderWidth), available.y / static_cast<float>(renderHeight));
    const ImVec2 imageSize(static_cast<float>(renderWidth) * fit, static_cast<float>(renderHeight) * fit);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (available.x - imageSize.x) * 0.5f,
        ImGui::GetCursorPosY() + (available.y - imageSize.y) * 0.5f));
    ImGui::Image(static_cast<ImTextureID>(descriptors_.Gpu(1).ptr), imageSize);
    const auto imagePosition = ImGui::GetItemRectMin();
    const ViewportRect image{imagePosition.x, imagePosition.y, imageSize.x, imageSize.y};
    const bool imageHovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const auto& io = ImGui::GetIO();
    auto camera = ViewCamera(world);
    auto& registry = world.Registry();
    const bool placingObject = objectPalette_.Dragging();
    bool captured = gizmo_.dragging || placingObject;
    // 图像共用一个拖放目标，材质分配和物体放置各自提交独立的作者事务。
    if (!gizmo_.dragging && !cameraDragging_ && ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(objectPresetPayload,
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            payload && payload->DataSize == sizeof(std::uint32_t) && objectPalette_.AllowDrop()) {
            captured = true;
            std::uint32_t index{};
            std::memcpy(&index, payload->Data, sizeof(index));
            if (index < world.ObjectPresets().size()) {
                const auto ray = ViewportRay(image, io.MousePos.x, io.MousePos.y, camera.transform, camera.camera);
                const auto placement = ray ? ResolveObjectPlacement(source, world, world.ObjectPresets()[index], *ray,
                    camera.transform, gizmo_.snap || io.KeyCtrl ? gizmo_.translationStep : 0, &surfacePicker_) : std::nullopt;
                if (placement) {
                    DrawObjectPlacementPreview(*placement, world.ObjectPresets()[index], source, image, camera);
                    if (payload->IsDelivery()) CreatePlacedObject(settings, world, index, placement->transform);
                }
            }
        }
        // 与资源浏览器使用同一材质协议，并在接收前检查槽位负载的大小。
        if (const auto* payload = ImGui::AcceptDragDropPayload("ENCORE_MATERIAL");
            payload && payload->DataSize == sizeof(std::uint32_t)) {
            std::uint32_t index{};
            std::memcpy(&index, payload->Data, sizeof(index));
            if (const auto ray = ViewportRay(image, io.MousePos.x, io.MousePos.y, camera.transform, camera.camera)) {
                // 与选择和放置共用接受快照，导航后投放材质不再重复复制整个场景。
                const auto target = PickEntity(source, world, *ray, &surfacePicker_);
                const auto before = CaptureEditorDocument(world, settings, selection_);
                if (ApplyMaterialAsset(world, target, index, settings)) {
                    selection_ = GizmoTarget(world, target);
                    CancelGizmo();
                    gizmo_.target = selection_;
                    history_.Record(before, CaptureEditorDocument(world, settings, selection_), "分配材质");
                    propertyActiveId_ = propertyOwner_ = 0;
                    propertyRecorded_ = false;
                    captured = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmo_.cancelled = false;

    // 工具快捷键限于视口，不消费相机移动或属性文本输入。
    if (focused && !captured && !io.WantTextInput && !ImGui::IsAnyItemActive() && !cameraDragging_ && !gizmo_.dragging &&
        !io.KeyCtrl && !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) gizmo_.operation = GizmoOperation::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_.operation = GizmoOperation::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_.operation = GizmoOperation::Rotate;
        const bool light = registry.valid(selection_) && registry.all_of<gameplay::Light>(selection_);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !light) gizmo_.operation = GizmoOperation::Scale;
        // 空格循环变换模式；选择模式进入移动，光源不提供无效的缩放模式。
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
            gizmo_.operation = gizmo_.operation == GizmoOperation::Translate ? GizmoOperation::Rotate :
                gizmo_.operation == GizmoOperation::Rotate && !light ? GizmoOperation::Scale : GizmoOperation::Translate;
    }
    if (gizmo_.dragging && (ImGui::IsKeyPressed(ImGuiKey_Escape) || !focused)) {
        // Escape 回滚完整拖动；失焦则提交最近有效位姿并释放捕获。
        if (focused && registry.valid(selection_)) {
            registry.get<gameplay::Transform>(selection_) = gizmo_.start;
            MarkManipulated(settings, world);
            gizmoBefore_.reset();
        }
        CancelGizmo();
        gizmo_.target = selection_;
        captured = true;
    }

    bool hoveredHandle = false;
    if (!placingObject && registry.valid(selection_) && selection_ != camera.entity && !gizmo_.cancelled && gizmo_.operation != GizmoOperation::Select) {
        auto& transform = registry.get<gameplay::Transform>(selection_);
        const bool light = registry.all_of<gameplay::Light>(selection_);
        const auto* mesh = registry.try_get<gameplay::MeshRenderer>(selection_);
        // ClearSelection 前导入源包围盒保持不变，每次选择只扫描大型网格一次。
        if (!gizmo_.pivotCached) {
            gizmo_.pivot = mesh ? MeshPivot(source, *mesh) : Vec3{};
            gizmo_.pivotCached = true;
        }
        const auto pivot = gizmo_.pivot;
        auto pose = transform;
        if (light) pose.scale = {1, 1, 1};
        auto matrix = GizmoPose(pose, pivot);
        const auto view = MakeGizmoView(camera.transform, camera.camera, static_cast<float>(renderWidth) / static_cast<float>(renderHeight));
        const auto basis = MakeCameraBasis(camera.transform.rotationDegrees.x, camera.transform.rotationDegrees.y, camera.transform.rotationDegrees.z);
        const float depth = Dot(Vec3{matrix[12], matrix[13], matrix[14]} - camera.transform.position, basis.forward);
        const float clipDepth = view.projection[10] * depth + view.projection[14];
        // 避免在近裁剪面后调用 ImGuizmo，其提前返回路径会留下未弹出的绘制裁剪。
        if (clipDepth >= 0.001f && depth < camera.camera.farPlane && GizmoTransform(matrix, pivot, pose)) {
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
            ImGuizmo::SetRect(image.x, image.y, image.width, image.height);
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetGizmoSizeClipSpace(0.15f);
            ImGuizmo::PushID("EditorTransform");
            // 无所属控件的图像按压可能留下视口 MoveId，已有变换拖动保持优先权。
            ImGuizmo::Enable(!cameraDragging_ && (gizmo_.dragging || (!io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
                !ImGui::IsMouseDown(ImGuiMouseButton_Middle))) && !io.WantTextInput && (!ImGui::IsAnyItemActive() || gizmo_.dragging) &&
                (imageHovered || gizmo_.dragging));
            const auto operation = gizmo_.operation == GizmoOperation::Translate ? ImGuizmo::TRANSLATE :
                gizmo_.operation == GizmoOperation::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
            const float step = gizmo_.operation == GizmoOperation::Translate ? gizmo_.translationStep :
                gizmo_.operation == GizmoOperation::Rotate ? gizmo_.rotationStep : gizmo_.scaleStep;
            const float snap[]{step, step, step};
            const bool changed = ImGuizmo::Manipulate(view.view.data(), view.projection.data(), operation,
                gizmo_.local || gizmo_.operation == GizmoOperation::Scale ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                matrix.data(), nullptr, gizmo_.snap || io.KeyCtrl ? snap : nullptr);
            const bool usingHandle = ImGuizmo::IsUsing();
            // 工具包通用 IsOver 使用上帧悬停，当前手柄类型可避免首次点击误选。
            hoveredHandle = ImGuizmo::GetHoveredHandleType() != ImGuizmo::MT_NONE;
            if (usingHandle && !gizmo_.dragging) {
                gizmo_.start = transform;
                BeginGizmoTransaction(settings, world);
            }
            captured = captured || usingHandle;
            gizmo_.dragging = usingHandle;
            ImGuizmo::PopID();
            if (changed) {
                if (auto edited = GizmoTransform(matrix, pivot, pose)) {
                    if (light) edited->scale = transform.scale;
                    if (PoseChanged(transform, *edited)) {
                        transform = *edited;
                        MarkManipulated(settings, world);
                    }
                }
            }
        } else if (gizmo_.dragging) {
            CancelGizmo();
            gizmo_.target = selection_;
        }
    }

    // 旋转或缩放跨入其他窗口后不再更新，在那里释放鼠标仍须结束捕获。
    if (gizmo_.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        CancelGizmo();
        gizmo_.target = selection_;
        captured = true;
    }
    if (!gizmo_.dragging && gizmoBefore_) CommitGizmoTransaction(settings, world);

    // 拾取共用显示图像的精确矩形，不与变换或右键相机拖动竞争。
    if (imageHovered && !captured && !hoveredHandle && !gizmo_.cancelled && !cameraDragging_ && !io.WantTextInput &&
        !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (const auto ray = ViewportRay(image, io.MousePos.x, io.MousePos.y, camera.transform, camera.camera)) {
            // 射线随相机变化重新求交，未变化的世界空间几何继续复用。
            selection_ = PickEntity(source, world, *ray, &surfacePicker_);
            CancelGizmo();
            gizmo_.target = selection_;
        }
    }
    NavigateViewport(settings, world, source, image, static_cast<float>(renderWidth) / static_cast<float>(renderHeight),
        delta, imageHovered, focused, captured);
}

void EditorUi::CreatePlacedObject(RenderSettings& settings, gameplay::World& world, std::uint32_t preset,
    const gameplay::Transform& transform) {
    const auto before = CaptureEditorDocument(world, settings, selection_);
    try {
        // 后端在创建前验证资源与光源预算；成功后一次记录实体、身份、选择和组件。
        const auto entity = world.CreatePresetObject(preset, transform);
        if (!world.Registry().valid(entity)) throw std::runtime_error("对象预设未创建有效实体");
        selection_ = entity;
        defaults_.Observe(world);
        settings.reset = true;
        CancelGizmo();
        gizmo_.target = selection_;
        history_.Record(before, CaptureEditorDocument(world, settings, selection_), "创建物体");
        propertyActiveId_ = propertyOwner_ = 0;
        propertyRecorded_ = false;
        objectPalette_.SetError({});
    } catch (const std::exception& error) {
        // 创建失败保留当前选择与已有历史，面板持续显示可读的后端诊断。
        objectPalette_.SetError(error.what());
    }
}
}
