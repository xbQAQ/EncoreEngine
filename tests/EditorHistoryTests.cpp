#include "editor/EditorHistory.h"
#include "render/Scene.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using render::editor::CaptureEditorDocument;
using render::editor::EditorHistory;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TransformDragIsOneStep() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Box");
    EditorHistory history;
    const auto original = CaptureEditorDocument(world, settings, selection);
    // 连续拖动记录增量位姿，但对用户表现为一次编辑事务。
    world.Registry().get<gameplay::Transform>(selection).position.x = 2;
    auto previous = CaptureEditorDocument(world, settings, selection);
    Check(history.Record(original, previous, "Move"), "first transform edit records a transaction");
    world.Registry().get<gameplay::Transform>(selection).position.x = 7;
    Check(history.Record(previous, CaptureEditorDocument(world, settings, selection), "Move", true), "drag extends the transaction");
    settings.reset = false;
    const auto revision = world.Revision();
    Check(history.UndoLabel() == "Move" && history.Undo(world, settings, selection), "undo identifies the action");
    Check(world.Registry().get<gameplay::Transform>(selection).position.x == 0, "merged drag restores the original pose");
    Check(!history.CanUndo() && history.CanRedo(), "one undo consumes the whole drag");
    Check(world.Revision() > revision && settings.reset, "replay invalidates renderer history");
    Check(history.RedoLabel() == "Move" && history.Redo(world, settings, selection), "redo identifies the action");
    Check(world.Registry().get<gameplay::Transform>(selection).position.x == 7, "redo applies the final pose");
}

void NoOpPreservesRedoAndRuntimeFlags() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Box");
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    settings.exposure = 3;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Exposure");
    Check(history.Undo(world, settings, selection), "exposure edit can be undone");
    // 运行时开关和选择变化不会创建编辑历史分支。
    before = CaptureEditorDocument(world, settings, selection);
    settings.reset = !settings.reset;
    settings.simulate = true;
    selection = entt::null;
    Check(!history.Record(before, CaptureEditorDocument(world, settings, selection), "No-op"), "runtime flags and selection alone create no entry");
    Check(history.CanRedo() && history.Redo(world, settings, selection), "no-op retains the redo branch");
    Check(settings.exposure == 3 && settings.simulate, "replay preserves the current simulation state");
}

void NewEditsDiscardRedo() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Box");
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    settings.bloom = 0.5f;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Bloom");
    Check(history.Undo(world, settings, selection), "initial branch is undoable");
    // 撤销后的真实编辑只替换被放弃的后续分支。
    before = CaptureEditorDocument(world, settings, selection);
    settings.exposure = 4;
    Check(history.Record(before, CaptureEditorDocument(world, settings, selection), "Exposure"), "new branch records");
    Check(!history.CanRedo() && !history.Redo(world, settings, selection), "abandoned future is inaccessible");
    Check(history.Undo(world, settings, selection) && settings.exposure == 0, "new branch remains undoable");
}

void ComponentsMaterialsAndSettingsRoundTrip() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    const auto lightEntity = world.LightEntity();
    const auto cameraEntity = world.CameraEntity();
    auto& registry = world.Registry();
    auto selection = *registry.view<gameplay::MeshRenderer>().begin();
    const auto meshEntity = selection;
    const auto materialHandle = world.MaterialHandle(0);
    const auto* material = world.Resources().Resolve(materialHandle);
    const auto* meshResource = world.Resources().Resolve(registry.get<gameplay::MeshRenderer>(meshEntity).meshResource);
    const auto* triangles = meshResource->Triangles().data();
    const auto resources = world.Resources().Size();
    const auto roughness = world.Materials()[0].baseRoughness.w;
    const auto name = registry.get<gameplay::Name>(meshEntity).value;
    const auto oldLight = registry.get<gameplay::Light>(lightEntity);
    const auto oldCamera = registry.get<gameplay::Camera>(cameraEntity);
    const auto oldPipeline = settings.pipeline;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    // 一次事务可以同时覆盖编辑组件、权威材质值和渲染控制项。
    registry.get<gameplay::Name>(meshEntity).value = "Edited mesh";
    registry.get<gameplay::MeshRenderer>(meshEntity).visible = false;
    registry.get<gameplay::Camera>(cameraEntity).verticalFovDegrees = 71;
    registry.get<gameplay::Light>(lightEntity).intensity = 42;
    registry.get<gameplay::Light>(lightEntity).type = gameplay::LightType::Spot;
    registry.emplace<gameplay::Velocity>(meshEntity, render::Vec3{1, 2, 3});
    world.Materials()[0].baseRoughness.w = 0.23f;
    settings.shadowBias = 0.08f;
    settings.pathVertices = 9;
    settings.pipeline = oldPipeline == 0 ? 1u : 0u;
    // 独立效果的开关与参数属于同一作者事务，不能只恢复其中一半。
    settings.ssao = settings.ssr = settings.ssil = settings.sss = settings.sky = true;
    settings.ssaoIntensity = 2; settings.ssaoRadius = 0.7f;
    settings.ssrIntensity = 0.5f; settings.ssrDistance = 8;
    settings.ssilIntensity = 0.75f;
    settings.sssIntensity = 1.5f; settings.sssRadius = 0.1f;
    settings.skyIntensity = 2.5f;
    selection = lightEntity;
    Check(history.Record(before, CaptureEditorDocument(world, settings, selection), "Properties"), "mixed edit records");
    Check(history.Undo(world, settings, selection), "mixed edit undoes");
    Check(selection == meshEntity && registry.get<gameplay::Name>(meshEntity).value == name, "selection and name restored");
    Check(registry.get<gameplay::MeshRenderer>(meshEntity).visible && !registry.all_of<gameplay::Velocity>(meshEntity), "visibility and missing velocity restored");
    Check(registry.get<gameplay::Camera>(cameraEntity).verticalFovDegrees == oldCamera.verticalFovDegrees, "camera projection restored");
    Check(registry.get<gameplay::Light>(lightEntity).intensity == oldLight.intensity && registry.get<gameplay::Light>(lightEntity).type == oldLight.type, "light values restored");
    Check(world.Materials()[0].baseRoughness.w == roughness && material->Parameters().baseRoughness.w == roughness, "undo writes authoritative material storage");
    Check(settings.shadowBias == 0.003f && settings.pathVertices == 5 && settings.pipeline == oldPipeline,
        "render controls and selected render pipeline restored");
    Check(!settings.ssao && !settings.ssr && !settings.ssil && !settings.sss && !settings.sky &&
        settings.ssaoIntensity == 1 && settings.ssaoRadius == 0.5f && settings.ssrIntensity == 1 && settings.ssrDistance == 6 &&
        settings.ssilIntensity == 1 && settings.sssIntensity == 1 && settings.sssRadius == 0.05f && settings.skyIntensity == 1,
        "effect switches and every parameter restore their defaults together");
    Check(history.Redo(world, settings, selection), "mixed edit redoes");
    Check(selection == lightEntity && registry.get<gameplay::Name>(meshEntity).value == "Edited mesh", "redo selection and name");
    Check(!registry.get<gameplay::MeshRenderer>(meshEntity).visible && registry.get<gameplay::Velocity>(meshEntity).linear.z == 3, "redo visibility and optional component");
    Check(material->Parameters().baseRoughness.w == 0.23f && settings.pathVertices == 9 && settings.pipeline != oldPipeline,
        "redo authoritative materials, controls and selected render pipeline");
    Check(settings.ssao && settings.ssr && settings.ssil && settings.sss && settings.sky &&
        settings.ssaoIntensity == 2 && settings.ssaoRadius == 0.7f && settings.ssrIntensity == 0.5f && settings.ssrDistance == 8 &&
        settings.ssilIntensity == 0.75f && settings.sssIntensity == 1.5f && settings.sssRadius == 0.1f && settings.skyIntensity == 2.5f,
        "effect switches and every parameter redo as one authored transaction");
    Check(world.Resources().Size() == resources && world.Resources().Resolve(materialHandle) == material, "history does not replace resource identities");
    Check(world.Resources().Resolve(registry.get<gameplay::MeshRenderer>(meshEntity).meshResource) == meshResource && meshResource->Triangles().data() == triangles, "mesh geometry ownership and allocation stay stable");
}

void UntouchedNavigationAndSettingsSurviveReplay() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    auto selection = world.LightEntity();
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    world.Registry().get<gameplay::Light>(selection).intensity = 36;
    settings.bloom = 0.4f;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Light");
    // 视口导航和无关控制项不属于本次事务的稀疏差异。
    auto& camera = world.Registry().get<gameplay::Transform>(world.CameraEntity());
    camera.position = {9, 8, 7};
    settings.exposure = 5;
    Check(history.Undo(world, settings, selection), "light edit undoes after navigation");
    Check(camera.position.x == 9 && camera.position.z == 7 && settings.exposure == 5, "unrelated live values survive undo");
    Check(history.Redo(world, settings, selection) && camera.position.y == 8 && settings.exposure == 5, "unrelated values survive redo");
}

void DeleteAndRestorePrimaryHandles() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    const auto light = world.LightEntity();
    const auto camera = world.CameraEntity();
    auto selection = light;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    // 精确的带代际标识保证结构撤销后 World 缓存句柄仍然有效。
    world.DestroyEntity(light);
    world.DestroyEntity(camera);
    selection = entt::null;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Delete");
    Check(world.LightEntity() == entt::null && world.CameraEntity() == entt::null, "deleted primaries are invalid");
    Check(history.Undo(world, settings, selection), "primary deletion undoes");
    Check(world.LightEntity() == light && world.CameraEntity() == camera && selection == light, "restoration revives exact primary identities");
    Check(history.Redo(world, settings, selection) && !world.Registry().valid(light) && !world.Registry().valid(camera), "primary deletion redoes");
    Check(selection == entt::null && history.Undo(world, settings, selection), "repeated structural replay remains stable");
    Check(world.LightEntity() == light && world.CameraEntity() == camera, "primary cache identity remains usable after repeated undo");
}

void CreatedLightRestoresOptionalAbsence() {
    gameplay::World world;
    render::RenderSettings settings;
    gameplay::Entity selection = entt::null;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    // 通过注册表创建的实体可以合法地缺少任意受支持组件。
    const auto light = world.Registry().create();
    world.Registry().emplace<gameplay::Light>(light).intensity = 27;
    selection = light;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Add light");
    Check(history.Undo(world, settings, selection) && !world.Registry().valid(light), "created light is removed by undo");
    Check(selection == entt::null && history.Redo(world, settings, selection), "created light redo restores selection");
    Check(selection == light && world.Registry().get<gameplay::Light>(light).intensity == 27, "created light restores exact identity and values");
    Check(!world.Registry().all_of<gameplay::Name>(light) && !world.Registry().all_of<gameplay::Transform>(light), "restore does not invent absent components");
}

void SlotReuseWithinOneTransaction() {
    gameplay::World world;
    render::RenderSettings settings;
    const auto oldEntity = world.CreateEntity("Old");
    world.Registry().emplace<gameplay::Light>(oldEntity);
    auto selection = oldEntity;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    // 删除和创建操作可以复用索引，同时保留不同代际。
    world.DestroyEntity(oldEntity);
    const auto newEntity = world.CreateEntity("New");
    world.Registry().emplace<gameplay::Camera>(newEntity);
    Check(entt::to_entity(oldEntity) == entt::to_entity(newEntity) && oldEntity != newEntity, "test exercises one reused slot");
    selection = newEntity;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Replace");
    Check(history.Undo(world, settings, selection), "slot replacement undoes");
    Check(selection == oldEntity && world.Registry().valid(oldEntity) && !world.Registry().valid(newEntity), "undo restores the old generation");
    Check(world.Registry().all_of<gameplay::Light>(oldEntity) && !world.Registry().all_of<gameplay::Camera>(oldEntity), "undo restores the old component set");
    Check(history.Redo(world, settings, selection) && selection == newEntity && !world.Registry().valid(oldEntity), "redo restores the new generation");
    Check(world.Registry().all_of<gameplay::Camera>(newEntity) && !world.Registry().all_of<gameplay::Light>(newEntity), "redo restores the new component set");
}

void OccupiedSlotRejectsWithoutPartialReplay() {
    gameplay::World world;
    render::RenderSettings settings;
    const auto removed = world.CreateEntity("Deleted");
    const auto untouched = world.CreateEntity("Keep");
    auto selection = removed;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    world.Registry().get<gameplay::Transform>(untouched).position.x = 4;
    world.DestroyEntity(removed);
    selection = entt::null;
    settings.exposure = 2;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Delete and edit");
    const auto intruder = world.CreateEntity("External");
    Check(entt::to_entity(intruder) == entt::to_entity(removed), "external edit occupies the restore slot");
    const auto count = world.Registry().storage<gameplay::Entity>().free_list();
    const auto revision = world.Revision();
    settings.reset = false;
    // 标识冲突校验必须早于实体删除、组件写入和设置回放。
    Check(!history.Undo(world, settings, selection), "identity conflict rejects undo");
    Check(world.Registry().valid(intruder) && !world.Registry().valid(removed), "conflict never clobbers external entities");
    Check(world.Registry().storage<gameplay::Entity>().free_list() == count, "conflict creates no leaked fallback entity");
    Check(world.Registry().get<gameplay::Transform>(untouched).position.x == 4 && settings.exposure == 2, "conflict makes no partial authoring changes");
    Check(world.Revision() == revision && !settings.reset && history.CanUndo() && !history.CanRedo(), "conflict preserves timeline and renderer state");
}

void MergedStructuralConflictPreflightsEveryStep() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Original");
    const auto original = selection;
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    world.DestroyEntity(original);
    selection = entt::null;
    auto after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Interaction");
    // 最后一个增量本身安全，但较早的恢复步骤会与外部实体冲突。
    const auto intruder = world.CreateEntity("External");
    before = CaptureEditorDocument(world, settings, selection);
    settings.bloom = 0.8f;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Interaction", true);
    Check(!history.Undo(world, settings, selection), "all merged steps are preflighted before replay");
    Check(world.Registry().valid(intruder) && !world.Registry().valid(original) && settings.bloom == 0.8f, "earlier conflict prevents later-step partial undo");
}

void InvalidSelectionIsCleared() {
    gameplay::World world;
    render::RenderSettings settings;
    const auto selected = world.CreateEntity("Selection");
    const auto edited = world.CreateEntity("Edited");
    auto selection = selected;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    world.Registry().get<gameplay::Name>(edited).value = "Changed";
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Rename");
    world.DestroyEntity(selected);
    Check(history.Undo(world, settings, selection) && selection == entt::null, "stale recorded selection is cleared during replay");
}

void RemovedComponentsAndMaterialLanesRoundTrip() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    auto selection = world.LightEntity();
    auto& registry = world.Registry();
    registry.emplace<gameplay::Velocity>(selection, render::Vec3{2, 3, 4});
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    // 删除组件不等于清零数据，材质编辑应保留无关的实时参数通道。
    registry.remove<gameplay::Name>(selection);
    registry.remove<gameplay::Transform>(selection);
    registry.remove<gameplay::Light>(selection);
    registry.get<gameplay::Velocity>(selection).linear = {-1, -2, -3};
    world.Materials()[0].extras.w = 0.7f;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Remove components");
    world.Materials()[0].principled.x = 0.89f;
    Check(history.Undo(world, settings, selection), "component removal undoes");
    Check(registry.all_of<gameplay::Name, gameplay::Transform, gameplay::Light>(selection), "removed components return");
    Check(registry.get<gameplay::Velocity>(selection).linear.y == 3, "existing velocity values restore");
    Check(world.Materials()[0].extras.w == 1 && world.Materials()[0].principled.x == 0.89f, "undo changes only the edited material lane");
    Check(history.Redo(world, settings, selection), "component removal redoes");
    Check(!registry.any_of<gameplay::Name, gameplay::Transform, gameplay::Light>(selection), "redo preserves removed component absence");
    Check(registry.get<gameplay::Velocity>(selection).linear.z == -3 && world.Materials()[0].extras.w == 0.7f, "redo restores edited existing values");
    Check(world.Materials()[0].principled.x == 0.89f, "redo preserves unrelated material lanes");
}

void RedoConflictPreservesTheFuture() {
    gameplay::World world;
    render::RenderSettings settings;
    gameplay::Entity selection = entt::null;
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    const auto created = world.CreateEntity("Created");
    selection = created;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Add");
    Check(history.Undo(world, settings, selection), "creation prepares redo");
    const auto intruder = world.CreateEntity("External");
    const auto revision = world.Revision();
    settings.reset = false;
    // 外部修改占用待创建实体的历史槽位时，同样通过预检保护重做。
    Check(!history.Redo(world, settings, selection), "redo rejects the externally occupied slot");
    Check(world.Registry().valid(intruder) && !world.Registry().valid(created), "redo rejection preserves the external generation");
    Check(history.CanRedo() && !history.CanUndo() && world.Revision() == revision && !settings.reset, "rejected redo preserves future and renderer state");
    world.DestroyEntity(intruder);
    Check(history.Redo(world, settings, selection) && selection == created, "redo succeeds after the conflict is removed");
}

void MergedStructuralStepsPreserveGenerations() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Old");
    const auto oldEntity = selection;
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    world.DestroyEntity(oldEntity);
    selection = entt::null;
    auto after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Replace");
    // 撤销时按相反顺序回放增量结构编辑。
    before = after;
    const auto replacement = world.CreateEntity("Replacement");
    selection = replacement;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Replace", true);
    Check(history.Undo(world, settings, selection) && selection == oldEntity, "merged replacement undoes to original generation");
    Check(world.Registry().valid(oldEntity) && !world.Registry().valid(replacement) && !history.CanUndo(), "one undo reverses all structural steps");
    Check(history.Redo(world, settings, selection) && selection == replacement, "merged replacement redoes to the replacement generation");
    Check(!world.Registry().valid(oldEntity) && world.Registry().valid(replacement), "merged redo retains one live generation");
}

void MergedPropertiesReturningToStartLeaveNoEntry() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    auto selection = world.LightEntity();
    auto& registry = world.Registry();
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    settings.bloom = 0.5f;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Earlier");
    const auto originalName = registry.get<gameplay::Name>(selection);
    const auto originalTransform = registry.get<gameplay::Transform>(selection);
    const auto originalLight = registry.get<gameplay::Light>(selection);
    const auto originalMaterial = world.Materials()[0];
    before = CaptureEditorDocument(world, settings, selection);
    // 文本框原生撤销或拖动回到起点时，取消整个活动编辑交互。
    registry.get<gameplay::Name>(selection).value = "Temporary";
    registry.get<gameplay::Transform>(selection).position.x = 4;
    registry.get<gameplay::Light>(selection).intensity = 20;
    registry.emplace<gameplay::Velocity>(selection, render::Vec3{1, 2, 3});
    world.Materials()[0].extras.w = 0.4f;
    settings.exposure = 3;
    auto after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Properties");
    before = after;
    registry.get<gameplay::Name>(selection) = originalName;
    registry.get<gameplay::Transform>(selection) = originalTransform;
    registry.get<gameplay::Light>(selection) = originalLight;
    registry.remove<gameplay::Velocity>(selection);
    world.Materials()[0] = originalMaterial;
    settings.exposure = 0;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Properties", true);
    Check(history.UndoLabel() == "Earlier", "net-zero interaction leaves no empty undo entry");
    Check(history.Undo(world, settings, selection) && settings.bloom == 0.06f, "next undo reaches the preceding real edit");
    Check(!history.CanUndo() && history.RedoLabel() == "Earlier", "cancelled interaction is absent from both timeline branches");
}

void CancelledFieldsBecomeSparseWithinMergedEdit() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Original");
    auto& registry = world.Registry();
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    registry.get<gameplay::Transform>(selection).position.x = 4;
    registry.get<gameplay::Name>(selection).value = "Renamed";
    settings.exposure = 3;
    auto after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Properties");
    before = after;
    registry.get<gameplay::Transform>(selection).position.x = 0;
    settings.exposure = 0;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Properties", true);
    // 即使其他字段仍使交互保持活动，被取消字段也必须退出差异记录。
    registry.get<gameplay::Transform>(selection).position.x = 9;
    settings.exposure = 5;
    Check(history.Undo(world, settings, selection), "remaining rename undoes");
    Check(registry.get<gameplay::Name>(selection).value == "Original", "net rename remains undoable");
    Check(registry.get<gameplay::Transform>(selection).position.x == 9 && settings.exposure == 5, "cancelled merged fields preserve later external values");
    Check(history.Redo(world, settings, selection) && registry.get<gameplay::Name>(selection).value == "Renamed", "remaining rename redoes");
    Check(registry.get<gameplay::Transform>(selection).position.x == 9 && settings.exposure == 5, "redo also excludes cancelled fields");
}

void MergedTransientEntityLeavesNoEntry() {
    gameplay::World world;
    render::RenderSettings settings;
    gameplay::Entity selection = entt::null;
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    const auto temporary = world.CreateEntity("Temporary");
    selection = temporary;
    world.Registry().emplace<gameplay::Light>(temporary);
    auto after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Create interaction");
    before = after;
    world.DestroyEntity(temporary);
    selection = entt::null;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Create interaction", true);
    // 两个编辑边界都不存在的临时代际，不产生可回放的编辑效果。
    Check(!history.CanUndo() && !history.CanRedo(), "created then deleted transient entity cancels its merged entry");
    Check(!history.Undo(world, settings, selection), "no transient structural undo remains");
}

void ContinuingCancelledInteractionDoesNotMergeOlderCommand() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Original");
    auto& name = world.Registry().get<gameplay::Name>(selection).value;
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    name = "Earlier";
    auto after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Properties");
    before = after;
    name = "Temporary";
    after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Properties");
    before = after;
    name = "Earlier";
    after = CaptureEditorDocument(world, settings, selection);
    history.Record(before, after, "Properties", true);
    // 控件取消编辑后仍可能保持活动，不能与更早的同名命令合并。
    before = after;
    name = "Continued";
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Properties", true);
    Check(history.Undo(world, settings, selection) && name == "Earlier", "continued edit does not merge an older interaction");
    Check(history.CanUndo() && history.Undo(world, settings, selection) && name == "Original", "older same-label command retains its own undo step");
}

void DiscontinuousMergedChangesRetainOrderedReplay() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Box");
    auto& transform = world.Registry().get<gameplay::Transform>(selection);
    EditorHistory history;
    auto before = CaptureEditorDocument(world, settings, selection);
    transform.position.x = 1;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Properties");
    // 外部移动会打断增量链，不能仅凭交互端点相同就取消事务。
    transform.position.x = 10;
    before = CaptureEditorDocument(world, settings, selection);
    transform.position.x = 0;
    history.Record(before, CaptureEditorDocument(world, settings, selection), "Properties", true);
    Check(history.CanUndo() && history.Undo(world, settings, selection), "discontinuous chain is not discarded as net-zero");
    Check(transform.position.x == 0 && history.Redo(world, settings, selection), "fallback preserves chronological undo and redo");
    Check(transform.position.x == 0 && !history.CanRedo(), "fallback consumes the whole merged interaction");
}

void HistoryIsBoundedAndClearable() {
    gameplay::World world;
    render::RenderSettings settings;
    auto selection = world.CreateEntity("Box");
    EditorHistory history;
    // 最旧编辑命令按容量上限过期，每项都不保存完整场景。
    for (int index = 0; index < 140; ++index) {
        const auto before = CaptureEditorDocument(world, settings, selection);
        settings.exposure = static_cast<float>(index + 1);
        history.Record(before, CaptureEditorDocument(world, settings, selection), "Exposure");
    }
    unsigned count = 0;
    while (history.Undo(world, settings, selection)) ++count;
    Check(count == 128 && settings.exposure == 12, "history retains exactly the most recent 128 transactions");
    history.Clear();
    Check(!history.CanUndo() && !history.CanRedo() && history.UndoLabel().empty() && history.RedoLabel().empty(), "clear removes both branches and labels");
}

void ScriptAuthoringRoundTrip() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    auto selection = world.CameraEntity();
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    world.Registry().emplace<gameplay::LuaScript>(selection, "function update(self,dt) end", "rotate.lua", false);
    auto* material = world.Material(0);
    material->ScriptSource() = "void Surface(inout Material m, float2 uv, float4 t) { m.baseRoughness.xyz=1; }";
    material->ScriptPath() = "surface.hlsl";
    Check(history.Record(before, CaptureEditorDocument(world, settings, selection), "Scripts"), "script authoring creates transaction");
    Check(history.Undo(world, settings, selection), "script source undo succeeds");
    Check(!world.Registry().all_of<gameplay::LuaScript>(selection) && material->ScriptSource().empty() && material->ScriptPath().empty(),
        "undo restores script component absence and empty material source");
    Check(history.Redo(world, settings, selection), "script source redo succeeds");
    const auto& script = world.Registry().get<gameplay::LuaScript>(selection);
    Check(script.path == "rotate.lua" && !script.enabled && !material->ScriptSource().empty() && material->ScriptPath() == "surface.hlsl",
        "redo restores authored source paths and execution switch");
    const auto second = CaptureEditorDocument(world, settings, selection);
    material->ScriptSource() += "\n// 下一次编辑";
    Check(history.Record(second, CaptureEditorDocument(world, settings, selection), "Scripts", true), "merged script source edit is recorded");
    Check(history.Undo(world, settings, selection) && material->ScriptSource().find("下一次编辑") == std::string::npos,
        "source text edits are undoable without copying executable state");
}
void SurfaceMaterialModeRoundTrip() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    render::RenderSettings settings;
    auto selection = *world.Registry().view<gameplay::MeshRenderer>().begin();
    auto& mesh = world.Registry().get<gameplay::MeshRenderer>(selection);
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, selection);
    // 材质模式单独切换也必须形成事务，不能因为网格和默认槽未变而被忽略。
    mesh.useSurfaceMaterials = true;
    Check(history.Record(before, CaptureEditorDocument(world, settings, selection), "Surface materials"), "surface mode edit records");
    Check(history.Undo(world, settings, selection) && !world.Registry().get<gameplay::MeshRenderer>(selection).useSurfaceMaterials,
        "undo restores whole model material override");
    Check(history.Redo(world, settings, selection) && world.Registry().get<gameplay::MeshRenderer>(selection).useSurfaceMaterials,
        "redo restores original surface materials");
}

void SerializedIdentityRoundTrip() {
    gameplay::World world;
    render::RenderSettings settings;
    auto entity = world.CreateEntity("Serialized object");
    world.Registry().emplace_or_replace<gameplay::ObjectIdentity>(entity, "level", "scene", "object");
    EditorHistory history;
    const auto before = CaptureEditorDocument(world, settings, entity);
    // 删除和恢复必须保留序列化身份，不能把运行时实体编号写成新的作者编号。
    world.DestroyEntity(entity);
    Check(history.Record(before, CaptureEditorDocument(world, settings, entt::null), "Delete"), "serialized deletion records");
    Check(history.Undo(world, settings, entity), "serialized deletion undoes");
    const auto& restored = world.Registry().get<gameplay::ObjectIdentity>(entity);
    Check(restored.levelId == "level" && restored.sceneId == "scene" && restored.objectId == "object",
        "undo restores level, scene and object identities");
    Check(history.Redo(world, settings, entity) && entity == entt::null, "serialized deletion redoes");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"merged transform transaction", TransformDragIsOneStep},
        {"no-op and runtime flags", NoOpPreservesRedoAndRuntimeFlags},
        {"new authoring branch", NewEditsDiscardRedo},
        {"components materials settings and resource ownership", ComponentsMaterialsAndSettingsRoundTrip},
        {"untouched navigation and settings", UntouchedNavigationAndSettingsSurviveReplay},
        {"primary entity deletion", DeleteAndRestorePrimaryHandles},
        {"created light and optional components", CreatedLightRestoresOptionalAbsence},
        {"generation reuse", SlotReuseWithinOneTransaction},
        {"occupied slot conflict", OccupiedSlotRejectsWithoutPartialReplay},
        {"merged conflict atomicity", MergedStructuralConflictPreflightsEveryStep},
        {"invalid selection", InvalidSelectionIsCleared},
        {"removed components and sparse materials", RemovedComponentsAndMaterialLanesRoundTrip},
        {"redo slot conflict", RedoConflictPreservesTheFuture},
        {"merged structural generations", MergedStructuralStepsPreserveGenerations},
        {"net-zero merged properties", MergedPropertiesReturningToStartLeaveNoEntry},
        {"sparse cancelled merged fields", CancelledFieldsBecomeSparseWithinMergedEdit},
        {"net-zero transient entity", MergedTransientEntityLeavesNoEntry},
        {"continued cancelled interaction boundary", ContinuingCancelledInteractionDoesNotMergeOlderCommand},
        {"discontinuous merged replay", DiscontinuousMergedChangesRetainOrderedReplay},
        {"bounded history and clear", HistoryIsBoundedAndClearable},
        {"script authoring round trip", ScriptAuthoringRoundTrip},
        {"surface material mode round trip", SurfaceMaterialModeRoundTrip},
        {"serialized object identity", SerializedIdentityRoundTrip},
    };
    int failed = 0;
    // 一次纯 CPU 执行展示全部行为失败，避免首个失败遮蔽后续结果。
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() - failed << '/' << tests.size() << " editor history tests passed\n";
    return failed ? 1 : 0;
}
