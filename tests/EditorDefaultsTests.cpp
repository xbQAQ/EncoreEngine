#include "editor/EditorDefaults.h"
#include "render/Scene.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using render::editor::EditorDefaults;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void ImportedValuesAndComponentPresence() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    auto& registry = world.Registry();
    const auto entity = *registry.view<gameplay::MeshRenderer>().begin();
    registry.get<gameplay::Transform>(entity).position = {2, 3, 4};
    registry.emplace<gameplay::Camera>(entity).verticalFovDegrees = 61;
    registry.emplace<gameplay::Light>(entity).intensity = 9;
    registry.emplace<gameplay::Velocity>(entity, render::Vec3{1, 2, 3});
    registry.emplace<gameplay::LuaScript>(entity, "function update() end", "initial.lua", true);
    registry.emplace_or_replace<gameplay::ObjectIdentity>(entity, "initial-level", "initial-scene", "initial-object");
    const auto mesh = registry.get<gameplay::MeshRenderer>(entity);
    const auto* meshOwner = world.Resources().Resolve(mesh.meshResource);
    const auto resourceCount = world.Resources().Size();
    EditorDefaults defaults;
    defaults.Capture(world);
    const auto name = defaults.EntityDefaults(entity)->name->value;
    // 恢复使用导入基线，并重新补回后续编辑删除的可选组件。
    registry.get<gameplay::Name>(entity).value = "Edited";
    registry.get<gameplay::Transform>(entity).position = {8, 8, 8};
    registry.get<gameplay::Transform>(entity).rotationDegrees.y = 42;
    registry.get<gameplay::Transform>(entity).scale.x = 2;
    registry.get<gameplay::MeshRenderer>(entity).visible = false;
    registry.remove<gameplay::Camera, gameplay::Velocity>(entity);
    registry.get<gameplay::Light>(entity).intensity = 100;
    registry.get<gameplay::LuaScript>(entity) = {"edited", "edited.lua", false};
    registry.get<gameplay::ObjectIdentity>(entity) = {"edited-level", "edited-scene", "edited-object"};
    defaults.Observe(world);
    const auto revision = world.Revision();
    Check(defaults.ResetEntity(world, entity), "edited entity restores its first baseline");
    Check(registry.valid(entity) && registry.get<gameplay::Name>(entity).value == name, "reset preserves identity and imported name");
    Check(registry.get<gameplay::Transform>(entity).position.x == 2 && registry.get<gameplay::Transform>(entity).position.z == 4,
        "imported pose replaces edited pose");
    Check(registry.get<gameplay::Transform>(entity).rotationDegrees.y == 0 && registry.get<gameplay::Transform>(entity).scale.x == 1,
        "rotation and scale restore baseline");
    Check(registry.get<gameplay::MeshRenderer>(entity).visible && registry.get<gameplay::Camera>(entity).verticalFovDegrees == 61,
        "mesh and optional camera restore imported values");
    Check(registry.get<gameplay::Light>(entity).intensity == 9 && registry.get<gameplay::Velocity>(entity).linear.z == 3,
        "light and velocity restore authored values");
    Check(registry.get<gameplay::LuaScript>(entity).source == "function update() end" &&
        registry.get<gameplay::LuaScript>(entity).path == "initial.lua" && registry.get<gameplay::LuaScript>(entity).enabled,
        "script source, path and enable state restore baseline");
    Check(registry.get<gameplay::ObjectIdentity>(entity).objectId == "initial-object" &&
        registry.get<gameplay::ObjectIdentity>(entity).sceneId == "initial-scene" &&
        registry.get<gameplay::ObjectIdentity>(entity).levelId == "initial-level", "serialized object identity restores baseline");
    Check(world.Revision() == revision + 1, "one real reset marks World dirty once");
    Check(world.Resources().Size() == resourceCount && world.Resources().Resolve(mesh.meshResource) == meshOwner,
        "reset retains borrowed mesh identity and resource ownership");
    Check(!defaults.ResetEntity(world, entity) && world.Revision() == revision + 1, "resetting unchanged values is a no-op");
}

void MissingComponentsRemainMissing() {
    gameplay::World world;
    const auto entity = world.Registry().create();
    EditorDefaults defaults;
    defaults.Capture(world);
    Check(defaults.EntityDefaults(entity) && !defaults.EntityDefaults(entity)->name, "component-free entity receives a baseline");
    // 恢复原始空对象时，移除创建后才添加的组件。
    world.Registry().emplace<gameplay::Name>(entity, "Temporary");
    world.Registry().emplace<gameplay::Transform>(entity);
    world.Registry().emplace<gameplay::Camera>(entity);
    world.Registry().emplace<gameplay::LuaScript>(entity);
    Check(defaults.ResetEntity(world, entity), "added authoring components can be reset away");
    Check(!world.Registry().any_of<gameplay::Name, gameplay::Transform, gameplay::Camera, gameplay::LuaScript>(entity),
        "reset restores missing component presence");
}

void CreatedObjectsAndGenerations() {
    gameplay::World world;
    EditorDefaults defaults;
    defaults.Capture(world);
    const auto first = world.CreateEntity("First");
    world.Registry().get<gameplay::Transform>(first).position.x = 4;
    defaults.Observe(world);
    world.Registry().get<gameplay::Transform>(first).position.x = 9;
    defaults.Observe(world);
    Check(defaults.ResetEntity(world, first) && world.Registry().get<gameplay::Transform>(first).position.x == 4,
        "first observation captures creation values exactly once");
    // EnTT 槽位被新代次复用时，不得应用已删除对象的默认值。
    world.DestroyEntity(first);
    const auto second = world.CreateEntity("Second");
    world.Registry().get<gameplay::Transform>(second).position.x = 12;
    defaults.Observe(world);
    Check(first != second && !defaults.ResetEntity(world, first), "deleted generation cannot mutate a reused slot");
    world.Registry().get<gameplay::Transform>(second).position.x = 20;
    Check(defaults.ResetEntity(world, second) && world.Registry().get<gameplay::Transform>(second).position.x == 12,
        "new generation has an independent creation baseline");
}

void MaterialsAndScriptsRetainOwnership() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    auto* material = world.Material(0);
    material->Parameters().baseRoughness.w = 0.63f;
    material->ScriptSource() = "original surface";
    material->ScriptPath() = "original.hlsl";
    const auto handle = world.MaterialHandle(0);
    EditorDefaults defaults;
    defaults.Capture(world);
    // 材质脚本和参数共用资源基线，所有权仍由原资源存储持有。
    material->Parameters().baseRoughness.w = 0.12f;
    material->Parameters().flags.x = 8;
    material->ScriptSource().clear();
    material->ScriptPath() = "edited.hlsl";
    defaults.Observe(world);
    Check(defaults.MaterialDefaults(0)->baseRoughness.w == 0.63f &&
        defaults.MaterialScriptDefaults(0)->source == "original surface", "observation never overwrites material baselines");
    const auto revision = world.Revision();
    Check(defaults.ResetMaterial(world, 0), "edited material resets its initial parameters and script");
    Check(world.Material(0) == material && world.MaterialHandle(0) == handle, "material reset preserves resource identity");
    Check(material->Parameters().baseRoughness.w == 0.63f && material->Parameters().flags.x == defaults.MaterialDefaults(0)->flags.x,
        "material factors and flags restore baseline");
    Check(material->ScriptSource() == "original surface" && material->ScriptPath() == "original.hlsl",
        "material script source and provenance restore baseline");
    Check(!defaults.ResetMaterial(world, 0) && world.Revision() == revision + 1, "unchanged material reset does not dirty the world");
    Check(!defaults.ResetMaterial(world, 9999) && !defaults.MaterialDefaults(9999), "invalid material indices are rejected");
}

void ResetIsUndoableWithoutChangingBaseline() {
    gameplay::World world;
    auto entity = world.CreateEntity("Object");
    render::RenderSettings settings;
    EditorDefaults defaults;
    defaults.Capture(world);
    world.Registry().get<gameplay::Transform>(entity).position.x = 7;
    const auto before = render::editor::CaptureEditorDocument(world, settings, entity);
    Check(defaults.ResetEntity(world, entity), "default reset applies");
    render::editor::EditorHistory history;
    // 恢复默认值是普通作者操作，撤销和重做不会改写创建基线。
    Check(history.Record(before, render::editor::CaptureEditorDocument(world, settings, entity), "Reset"), "reset can enter authoring history");
    Check(history.Undo(world, settings, entity) && world.Registry().get<gameplay::Transform>(entity).position.x == 7,
        "undo restores the value from before reset");
    defaults.Observe(world);
    Check(defaults.EntityDefaults(entity)->transform->position.x == 0, "undo does not overwrite baseline");
    Check(history.Redo(world, settings, entity) && world.Registry().get<gameplay::Transform>(entity).position.x == 0,
        "redo reapplies the baseline value");
}

void ClearAndSceneBoundaries() {
    gameplay::World first;
    first.CreateCornell(render::BuildCornellScene());
    const auto oldEntity = first.CameraEntity();
    EditorDefaults defaults;
    defaults.Capture(first);
    gameplay::World second;
    const auto other = second.CreateEntity("Other");
    Check(!defaults.ResetEntity(second, other), "another World cannot consume captured scene baselines");
    // 同一 World 内替换资源后，旧导入身份在重新捕获之前保持失效。
    first.CreateCornell(render::BuildCornellScene());
    Check(!defaults.ResetMaterial(first, 0) && !defaults.ResetEntity(first, oldEntity), "replaced scene cannot consume stale defaults");
    defaults.Observe(first);
    Check(defaults.MaterialDefaults(0), "observing replaced scene captures its new resource defaults");
    defaults.Clear();
    Check(!defaults.EntityDefaults(first.CameraEntity()) && !defaults.MaterialDefaults(0) && !defaults.MaterialScriptDefaults(0),
        "clear drops every borrowed baseline");
    Check(!defaults.ResetEntity(first, first.CameraEntity()), "cleared baselines cannot reset entities");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"imported values and component presence", ImportedValuesAndComponentPresence},
        {"missing components", MissingComponentsRemainMissing}, {"creation and generations", CreatedObjectsAndGenerations},
        {"material values and scripts", MaterialsAndScriptsRetainOwnership},
        {"undoable reset", ResetIsUndoableWithoutChangingBaseline}, {"scene boundaries and clear", ClearAndSceneBoundaries}};
    int failed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() - failed << '/' << tests.size() << " editor defaults tests passed\n";
    return failed ? 1 : 0;
}
