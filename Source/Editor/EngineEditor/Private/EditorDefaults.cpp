#include "editor/EditorDefaults.h"
#include <tuple>

namespace render::editor {
namespace {
// 比较具名字段，避免结构体填充字节影响是否需要恢复的判断。
bool Equal(const Vec3& a, const Vec3& b) { return std::tie(a.x, a.y, a.z) == std::tie(b.x, b.y, b.z); }
bool Equal(const Float4& a, const Float4& b) { return std::tie(a.x, a.y, a.z, a.w) == std::tie(b.x, b.y, b.z, b.w); }
bool Equal(const Uint4& a, const Uint4& b) { return std::tie(a.x, a.y, a.z, a.w) == std::tie(b.x, b.y, b.z, b.w); }
bool Equal(const gameplay::Name& a, const gameplay::Name& b) { return a.value == b.value; }
bool Equal(const gameplay::Transform& a, const gameplay::Transform& b) {
    return Equal(a.position, b.position) && Equal(a.rotationDegrees, b.rotationDegrees) && Equal(a.scale, b.scale);
}
bool Equal(const gameplay::MeshRenderer& a, const gameplay::MeshRenderer& b) {
    // 恢复默认值时一并恢复模型内部材质，避免只还原当前编辑槽。
    return std::tie(a.firstTriangle, a.triangleCount, a.materialIndex, a.visible, a.lightOwned, a.meshResource, a.useSurfaceMaterials) ==
        std::tie(b.firstTriangle, b.triangleCount, b.materialIndex, b.visible, b.lightOwned, b.meshResource, b.useSurfaceMaterials);
}
bool Equal(const gameplay::Camera& a, const gameplay::Camera& b) {
    return std::tie(a.verticalFovDegrees, a.nearPlane, a.farPlane, a.active) ==
        std::tie(b.verticalFovDegrees, b.nearPlane, b.farPlane, b.active);
}
bool Equal(const gameplay::Light& a, const gameplay::Light& b) {
    return Equal(a.color, b.color) && Equal(a.direction, b.direction) && Equal(a.tangent, b.tangent) && Equal(a.bitangent, b.bitangent) &&
        std::tie(a.type, a.intensity, a.width, a.height, a.range, a.enabled, a.sourceRadius,
            a.outerAngleDegrees, a.innerAngleDegrees, a.emitterMaterial) ==
        std::tie(b.type, b.intensity, b.width, b.height, b.range, b.enabled, b.sourceRadius,
            b.outerAngleDegrees, b.innerAngleDegrees, b.emitterMaterial);
}
bool Equal(const gameplay::Velocity& a, const gameplay::Velocity& b) { return Equal(a.linear, b.linear); }
bool Equal(const gameplay::LuaScript& a, const gameplay::LuaScript& b) { return a == b; }
bool Equal(const gameplay::ObjectIdentity& a, const gameplay::ObjectIdentity& b) { return a == b; }
bool Equal(const GpuMaterial& a, const GpuMaterial& b) {
    return Equal(a.baseRoughness, b.baseRoughness) && Equal(a.emissionMetallic, b.emissionMetallic) &&
        Equal(a.transmissionIor, b.transmissionIor) && Equal(a.principled, b.principled) && Equal(a.sheen, b.sheen) &&
        Equal(a.medium, b.medium) && Equal(a.flags, b.flags) && Equal(a.textures, b.textures) &&
        Equal(a.textures2, b.textures2) && Equal(a.extras, b.extras) && Equal(a.tangentFrame, b.tangentFrame);
}

// 组件是否存在同样属于创建基线；恢复时不创建实体，也不保留资源所有权。
template<class T> std::optional<T> Copy(const entt::registry& registry, gameplay::Entity entity) {
    if (const auto* value = registry.try_get<T>(entity)) return *value;
    return std::nullopt;
}
template<class T> bool Restore(entt::registry& registry, gameplay::Entity entity, const std::optional<T>& baseline) {
    const auto* current = registry.try_get<T>(entity);
    if (baseline) {
        if (current && Equal(*current, *baseline)) return false;
        registry.emplace_or_replace<T>(entity, *baseline);
        return true;
    }
    if (!current) return false;
    registry.remove<T>(entity);
    return true;
}
}

void EditorDefaults::Capture(const gameplay::World& world) {
    // 显式捕获建立新的场景边界，导入或加载场景后由编辑器调用。
    Clear();
    world_ = &world;
    Observe(world);
}

bool EditorDefaults::MatchesWorld(const gameplay::World& world) const {
    if (world_ != &world) return false;
    // 资源身份不会复用，因此相同 World 地址内的资源替换也能拒绝旧材质基线。
    for (const auto& [index, baseline] : materials_)
        if (index >= world.Materials().size() || world.MaterialHandle(index) != baseline.resource) return false;
    return true;
}

void EditorDefaults::Observe(const gameplay::World& world) {
    if (!MatchesWorld(world)) {
        Capture(world);
        return;
    }
    const auto& registry = world.Registry();
    const auto* storage = registry.storage<gameplay::Entity>();
    // 只复制首次出现的完整实体身份；后续编辑、撤销和模拟不会覆盖其创建值。
    if (storage) for (const auto [entity] : storage->each()) {
        if (entities_.contains(entity)) continue;
        entities_.emplace(entity, EditorEntitySnapshot{entity, Copy<gameplay::Name>(registry, entity),
            Copy<gameplay::Transform>(registry, entity), Copy<gameplay::MeshRenderer>(registry, entity),
            Copy<gameplay::Camera>(registry, entity), Copy<gameplay::Light>(registry, entity),
            Copy<gameplay::Velocity>(registry, entity), Copy<gameplay::LuaScript>(registry, entity),
            Copy<gameplay::ObjectIdentity>(registry, entity)});
    }
    // 材质只保留参数、源文本和非拥有句柄，不复制几何、纹理或材质资源对象。
    for (unsigned index = 0; index < world.Materials().size(); ++index) {
        if (materials_.contains(index)) continue;
        const auto* material = world.Material(index);
        if (!material) continue;
        materials_.emplace(index, MaterialBaseline{material->Parameters(),
            {index, material->ScriptSource(), material->ScriptPath()}, world.MaterialHandle(index)});
    }
}

void EditorDefaults::Clear() {
    // 丢弃旧场景基线不影响 World 拥有的任何资源。
    entities_.clear();
    materials_.clear();
    world_ = nullptr;
}

const EditorEntitySnapshot* EditorDefaults::EntityDefaults(gameplay::Entity entity) const {
    const auto found = entities_.find(entity);
    return found == entities_.end() ? nullptr : &found->second;
}

const GpuMaterial* EditorDefaults::MaterialDefaults(unsigned index) const {
    const auto found = materials_.find(index);
    return found == materials_.end() ? nullptr : &found->second.parameters;
}

const MaterialShaderSource* EditorDefaults::MaterialScriptDefaults(unsigned index) const {
    const auto found = materials_.find(index);
    return found == materials_.end() ? nullptr : &found->second.script;
}

bool EditorDefaults::ResetEntity(gameplay::World& world, gameplay::Entity entity) {
    const auto* baseline = EntityDefaults(entity);
    if (!baseline || !MatchesWorld(world) || !world.Registry().valid(entity)) return false;
    auto& registry = world.Registry();
    // 每类作者组件独立恢复，保留原实体身份和未参与编辑器的其他组件。
    bool changed = Restore(registry, entity, baseline->name);
    changed |= Restore(registry, entity, baseline->transform);
    changed |= Restore(registry, entity, baseline->mesh);
    changed |= Restore(registry, entity, baseline->camera);
    changed |= Restore(registry, entity, baseline->light);
    changed |= Restore(registry, entity, baseline->velocity);
    changed |= Restore(registry, entity, baseline->script);
    changed |= Restore(registry, entity, baseline->identity);
    if (changed) world.MarkDirty();
    return changed;
}

bool EditorDefaults::ResetMaterial(gameplay::World& world, unsigned index) {
    const auto found = materials_.find(index);
    if (found == materials_.end() || !MatchesWorld(world)) return false;
    auto* material = world.Material(index);
    if (!material) return false;
    const auto& baseline = found->second;
    // 比较完整作者值；无变化时不推进 revision，也不触发无意义的历史重建。
    if (Equal(material->Parameters(), baseline.parameters) && material->ScriptSource() == baseline.script.source &&
        material->ScriptPath() == baseline.script.path) return false;
    material->Parameters() = baseline.parameters;
    material->ScriptSource() = baseline.script.source;
    material->ScriptPath() = baseline.script.path;
    world.MarkDirty();
    return true;
}
}
