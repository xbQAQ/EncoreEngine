#include "editor/EditorHistory.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace render::editor {
namespace {
template<class T> bool Equal(const T& a, const T& b) { return a == b; }
bool Equal(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Equal(const Float4& a, const Float4& b) { return std::tie(a.x, a.y, a.z, a.w) == std::tie(b.x, b.y, b.z, b.w); }
bool Equal(const Uint4& a, const Uint4& b) { return std::tie(a.x, a.y, a.z, a.w) == std::tie(b.x, b.y, b.z, b.w); }
bool Equal(const gameplay::Name& a, const gameplay::Name& b) { return a.value == b.value; }
bool Equal(const gameplay::Transform& a, const gameplay::Transform& b) {
    return Equal(a.position, b.position) && Equal(a.rotationDegrees, b.rotationDegrees) && Equal(a.scale, b.scale);
}
bool Equal(const gameplay::MeshRenderer& a, const gameplay::MeshRenderer& b) {
    // 表面材质与统一覆盖的切换也是作者编辑，必须参与撤销和重做。
    return std::tie(a.firstTriangle, a.triangleCount, a.materialIndex, a.visible, a.lightOwned, a.meshResource, a.useSurfaceMaterials) ==
           std::tie(b.firstTriangle, b.triangleCount, b.materialIndex, b.visible, b.lightOwned, b.meshResource, b.useSurfaceMaterials);
}
bool Equal(const gameplay::Camera& a, const gameplay::Camera& b) {
    return std::tie(a.verticalFovDegrees, a.nearPlane, a.farPlane, a.active) ==
           std::tie(b.verticalFovDegrees, b.nearPlane, b.farPlane, b.active);
}
bool Equal(const gameplay::Light& a, const gameplay::Light& b) {
    return Equal(a.color, b.color) && Equal(a.direction, b.direction) && Equal(a.tangent, b.tangent) &&
           Equal(a.bitangent, b.bitangent) &&
           std::tie(a.type, a.intensity, a.width, a.height, a.range, a.enabled, a.sourceRadius,
                    a.outerAngleDegrees, a.innerAngleDegrees, a.emitterMaterial) ==
           std::tie(b.type, b.intensity, b.width, b.height, b.range, b.enabled, b.sourceRadius,
                    b.outerAngleDegrees, b.innerAngleDegrees, b.emitterMaterial);
}
bool Equal(const gameplay::Velocity& a, const gameplay::Velocity& b) { return Equal(a.linear, b.linear); }

// 组件是否存在也是其值的一部分，撤销不能凭空补造可选编辑数据。
template<class T> bool Equal(const std::optional<T>& a, const std::optional<T>& b) {
    return a.has_value() == b.has_value() && (!a || Equal(*a, *b));
}

template<class T> struct ValueChange {
    T before; // 本次增量编辑前的值或组件存在状态。
    T after; // 本次增量编辑后的值或组件存在状态。
};

template<class T> using ComponentChange = std::optional<ValueChange<std::optional<T>>>;

struct EntityChange {
    gameplay::Entity entity{entt::null}; // 此差异作用的精确实体代际。
    bool beforeExists{}; // 此标识在增量编辑前是否存在。
    bool afterExists{}; // 此标识在增量编辑后是否存在。
    ComponentChange<gameplay::Name> name; // 名称值或名称组件存在状态的变化。
    ComponentChange<gameplay::Transform> transform; // 位姿值或位姿组件存在状态的变化。
    ComponentChange<gameplay::MeshRenderer> mesh; // 网格编辑设置或组件存在状态的变化。
    ComponentChange<gameplay::Camera> camera; // 投影设置或相机组件存在状态的变化。
    ComponentChange<gameplay::Light> light; // 灯光参数或灯光组件存在状态的变化。
    ComponentChange<gameplay::Velocity> velocity; // 模拟设置或速度组件存在状态的变化。
    ComponentChange<gameplay::LuaScript> script; // 游戏逻辑源码或脚本组件存在状态的变化。
    ComponentChange<gameplay::ObjectIdentity> identity; // 关卡、场景及稳定对象身份或组件存在状态的变化。
};

template<class Owner, class T> struct FieldChange {
    T Owner::* member; // 类型安全的编辑字段，不比较字节偏移或结构体填充。
    T before; // 仅此字段修改前的值。
    T after; // 仅此字段修改后的值。
};

using SettingsChange = std::variant<FieldChange<RenderSettings, unsigned>, FieldChange<RenderSettings, bool>,
                                    FieldChange<RenderSettings, float>, FieldChange<RenderSettings, int>>;
using MaterialFieldChange = std::variant<FieldChange<GpuMaterial, Float4>, FieldChange<GpuMaterial, Uint4>>;

struct MaterialChange {
    std::size_t index{}; // World 中现有的权威材质槽，历史回放不创建资源。
    std::vector<MaterialFieldChange> fields; // 发生变化的 GPU 参数通道，不覆盖无关的实时通道。
};

struct Delta {
    std::vector<EntityChange> entities; // 变化的组件值及精确的结构标识转移。
    std::vector<MaterialChange> materials; // 不借用资源存储的稀疏参数编辑。
    std::vector<ValueChange<MaterialShaderSource>> scripts; // 按现有材质槽标识索引的源码变化。
    std::vector<SettingsChange> settings; // 变化的持久控制项，不包含 reset 和 simulate。
    gameplay::Entity selectionBefore{entt::null}; // 撤销本次编辑时恢复的选择。
    gameplay::Entity selectionAfter{entt::null}; // 重做本次编辑时恢复的选择。
};

struct Entry {
    std::string label; // 编辑器撤销和重做控件显示的命令名称。
    std::vector<Delta> deltas; // 一次交互中按顺序保存的增量编辑。
};

// 显式枚举类型安全的作者成员；管线选择参与撤销，运行时重置和模拟标志不参与。
constexpr auto SettingsMembers = std::tuple{
    &RenderSettings::pipeline, &RenderSettings::gi, &RenderSettings::backend, &RenderSettings::samples,
    &RenderSettings::temporalReuse, &RenderSettings::spatialReuse, &RenderSettings::shadows,
    &RenderSettings::shadowMode, &RenderSettings::shadowBias, &RenderSettings::shadowNormalBias,
    &RenderSettings::shadowRadius, &RenderSettings::vsmMinVariance, &RenderSettings::vsmBleedReduction,
    &RenderSettings::autoExposure, &RenderSettings::exposure, &RenderSettings::bloom,
    &RenderSettings::ssao, &RenderSettings::ssaoIntensity, &RenderSettings::ssaoRadius,
    &RenderSettings::ssr, &RenderSettings::ssrIntensity, &RenderSettings::ssrDistance,
    &RenderSettings::ssil, &RenderSettings::ssilIntensity,
    &RenderSettings::sss, &RenderSettings::sssIntensity, &RenderSettings::sssRadius,
    &RenderSettings::sky, &RenderSettings::skyIntensity,
    &RenderSettings::giIntensity, &RenderSettings::mediumDensity, &RenderSettings::phaseG,
    &RenderSettings::pathVertices, &RenderSettings::candidates, &RenderSettings::maxHistory,
    &RenderSettings::spatialRadius, &RenderSettings::debugView};
constexpr auto MaterialMembers = std::tuple{
    &GpuMaterial::baseRoughness, &GpuMaterial::emissionMetallic, &GpuMaterial::transmissionIor,
    &GpuMaterial::principled, &GpuMaterial::sheen, &GpuMaterial::medium, &GpuMaterial::flags,
    &GpuMaterial::textures, &GpuMaterial::textures2, &GpuMaterial::extras, &GpuMaterial::tangentFrame};

template<class T> std::optional<T> CopyComponent(const entt::registry& registry, gameplay::Entity entity) {
    if (const auto* value = registry.try_get<T>(entity)) return *value;
    return std::nullopt;
}

template<class T> ComponentChange<T> ChangedComponent(const std::optional<T>& before, const std::optional<T>& after) {
    if (Equal(before, after)) return std::nullopt;
    return ValueChange<std::optional<T>>{before, after};
}

template<class Variant, class Owner, class T>
void AddFieldChange(std::vector<Variant>& output, const Owner& before, const Owner& after, T Owner::* member) {
    if (!Equal(before.*member, after.*member)) output.emplace_back(FieldChange<Owner, T>{member, before.*member, after.*member});
}

bool Empty(const EntityChange& change) {
    return change.beforeExists == change.afterExists && !change.name && !change.transform && !change.mesh &&
           !change.camera && !change.light && !change.velocity && !change.script && !change.identity;
}
bool Empty(const Delta& delta) { return delta.entities.empty() && delta.materials.empty() && delta.settings.empty() && delta.scripts.empty(); }

Delta Difference(const EditorDocument& before, const EditorDocument& after) {
    Delta delta;
    delta.selectionBefore = before.selection;
    delta.selectionAfter = after.selection;
    std::map<gameplay::Entity, std::pair<const EditorEntitySnapshot*, const EditorEntitySnapshot*>> entities;
    for (const auto& entity : before.entities) entities[entity.entity].first = &entity;
    for (const auto& entity : after.entities) entities[entity.entity].second = &entity;
    // 完整标识可以区分复用同一 EnTT 索引的删除与创建操作。
    for (const auto& [entity, snapshots] : entities) {
        const auto& [oldSnapshot, newSnapshot] = snapshots;
        const EditorEntitySnapshot absent;
        const auto& oldValue = oldSnapshot ? *oldSnapshot : absent;
        const auto& newValue = newSnapshot ? *newSnapshot : absent;
        EntityChange change;
        change.entity = entity;
        change.beforeExists = oldSnapshot != nullptr;
        change.afterExists = newSnapshot != nullptr;
        change.name = ChangedComponent(oldValue.name, newValue.name);
        change.transform = ChangedComponent(oldValue.transform, newValue.transform);
        change.mesh = ChangedComponent(oldValue.mesh, newValue.mesh);
        change.camera = ChangedComponent(oldValue.camera, newValue.camera);
        change.light = ChangedComponent(oldValue.light, newValue.light);
        change.velocity = ChangedComponent(oldValue.velocity, newValue.velocity);
        change.script = ChangedComponent(oldValue.script, newValue.script);
        change.identity = ChangedComponent(oldValue.identity, newValue.identity);
        if (!Empty(change)) delta.entities.push_back(std::move(change));
    }
    // 资源表替换属于场景导入，普通编辑只修改现有槽位。
    for (std::size_t index = 0; index < std::min(before.materials.size(), after.materials.size()); ++index) {
        MaterialChange change;
        change.index = index;
        std::apply([&](auto... members) {
            (AddFieldChange(change.fields, before.materials[index], after.materials[index], members), ...);
        }, MaterialMembers);
        if (!change.fields.empty()) delta.materials.push_back(std::move(change));
    }
    std::apply([&](auto... members) { (AddFieldChange(delta.settings, before.settings, after.settings, members), ...); }, SettingsMembers);
    // 源码变化属于编辑事务；可执行管线及 Lua 虚拟机状态由各自系统重建。
    for (std::size_t index = 0; index < std::min(before.materialScripts.size(), after.materialScripts.size()); ++index)
        if (before.materialScripts[index] != after.materialScripts[index])
            delta.scripts.push_back({before.materialScripts[index], after.materialScripts[index]});
    return delta;
}

template<class T> bool ComposeComponent(ComponentChange<T>& combined, const ComponentChange<T>& next) {
    if (!next) return true;
    if (!combined) combined = next;
    else {
        // 不连续的值链可能包含外部编辑，必须保留按顺序回放的信息。
        if (!Equal(combined->after, next->before)) return false;
        combined->after = next->after;
    }
    if (Equal(combined->before, combined->after)) combined.reset();
    return true;
}

template<class Variant> bool ComposeFields(std::vector<Variant>& combined, const std::vector<Variant>& next) {
    for (const auto& field : next) {
        const bool continuous = std::visit([&](const auto& change) {
            using Change = std::decay_t<decltype(change)>;
            const auto found = std::find_if(combined.begin(), combined.end(), [&](const Variant& candidate) {
                const auto* previous = std::get_if<Change>(&candidate);
                return previous && previous->member == change.member;
            });
            if (found == combined.end()) combined.push_back(field);
            else {
                auto& previous = std::get<Change>(*found);
                if (!Equal(previous.after, change.before)) return false;
                previous.after = change.after;
                if (Equal(previous.before, previous.after)) combined.erase(found);
            }
            return true;
        }, field);
        if (!continuous) return false;
    }
    return true;
}

bool Compose(Delta& combined, const Delta& next) {
    // 按完整标识而非索引合并，确保替换代际仍是独立结构编辑。
    for (const auto& change : next.entities) {
        const auto found = std::find_if(combined.entities.begin(), combined.entities.end(), [&](const EntityChange& candidate) {
            return candidate.entity == change.entity;
        });
        if (found == combined.entities.end()) combined.entities.push_back(change);
        else {
            if (found->afterExists != change.beforeExists) return false;
            found->afterExists = change.afterExists;
            if (!ComposeComponent(found->name, change.name) || !ComposeComponent(found->transform, change.transform) ||
                !ComposeComponent(found->mesh, change.mesh) || !ComposeComponent(found->camera, change.camera) ||
                !ComposeComponent(found->light, change.light) || !ComposeComponent(found->velocity, change.velocity) ||
                !ComposeComponent(found->script, change.script) || !ComposeComponent(found->identity, change.identity)) return false;
            // 两个编辑边界都不存在的临时实体，不产生可撤销状态。
            if ((!found->beforeExists && !found->afterExists) || Empty(*found)) combined.entities.erase(found);
        }
    }
    for (const auto& change : next.materials) {
        const auto found = std::find_if(combined.materials.begin(), combined.materials.end(), [&](const MaterialChange& candidate) {
            return candidate.index == change.index;
        });
        if (found == combined.materials.end()) combined.materials.push_back(change);
        else {
            if (!ComposeFields(found->fields, change.fields)) return false;
            if (found->fields.empty()) combined.materials.erase(found);
        }
    }
    if (!ComposeFields(combined.settings, next.settings)) return false;
    for (const auto& change : next.scripts) {
        const auto found = std::find_if(combined.scripts.begin(), combined.scripts.end(), [&](const auto& existing) {
            return existing.before.materialIndex == change.before.materialIndex;
        });
        if (found == combined.scripts.end()) combined.scripts.push_back(change);
        else {
            if (found->after != change.before) return false;
            found->after = change.after;
            if (found->before == found->after) combined.scripts.erase(found);
        }
    }
    combined.selectionAfter = next.selectionAfter;
    return true;
}

void Extend(Entry& entry, Delta delta) {
    Delta combined = entry.deltas.front();
    // 在副本上尝试合并，遇到不连续变化时保留全部原始增量。
    for (std::size_t index = 1; index < entry.deltas.size(); ++index) {
        if (!Compose(combined, entry.deltas[index])) {
            entry.deltas.push_back(std::move(delta));
            return;
        }
    }
    if (!Compose(combined, delta)) {
        entry.deltas.push_back(std::move(delta));
        return;
    }
    entry.deltas.clear();
    if (!Empty(combined)) entry.deltas.push_back(std::move(combined));
}

template<class Function> void VisitDeltas(const Entry& entry, bool forward, Function&& function) {
    if (forward) {
        for (const auto& delta : entry.deltas) function(delta);
    } else {
        for (auto it = entry.deltas.rbegin(); it != entry.deltas.rend(); ++it) function(*it);
    }
}

bool Preflight(const Entry& entry, const gameplay::World& world, bool forward) {
    std::unordered_map<entt::id_type, gameplay::Entity> occupied;
    const auto* entities = world.Registry().storage<gameplay::Entity>();
    occupied.reserve(entities->free_list());
    for (const auto [entity] : entities->each()) occupied.emplace(entt::to_entity(entity), entity);
    bool valid = true;
    // 正式修改前模拟完整事务组，包括较早合并步骤中的标识冲突。
    VisitDeltas(entry, forward, [&](const Delta& delta) {
        if (!valid) return;
        for (const auto& material : delta.materials) {
            if (material.index >= world.Materials().size()) { valid = false; return; }
        }
        for (const auto& script : delta.scripts)
            if (!world.Material(script.before.materialIndex)) { valid = false; return; }
        for (const auto& change : delta.entities) {
            const bool sourceExists = forward ? change.beforeExists : change.afterExists;
            const auto found = occupied.find(entt::to_entity(change.entity));
            if (sourceExists && (found == occupied.end() || found->second != change.entity)) { valid = false; return; }
        }
        // 先移除全部离开的代际，使一次事务可以替换复用槽位。
        for (const auto& change : delta.entities) {
            const bool sourceExists = forward ? change.beforeExists : change.afterExists;
            const bool targetExists = forward ? change.afterExists : change.beforeExists;
            if (sourceExists && !targetExists) occupied.erase(entt::to_entity(change.entity));
        }
        for (const auto& change : delta.entities) {
            const bool sourceExists = forward ? change.beforeExists : change.afterExists;
            const bool targetExists = forward ? change.afterExists : change.beforeExists;
            if (!sourceExists && targetExists && !occupied.emplace(entt::to_entity(change.entity), change.entity).second) {
                valid = false;
                return;
            }
        }
    });
    return valid;
}

template<class T> void ApplyComponent(entt::registry& registry, gameplay::Entity entity,
                                      const ComponentChange<T>& change, bool forward) {
    if (!change) return;
    const auto& value = forward ? change->after : change->before;
    if (value) registry.emplace_or_replace<T>(entity, *value);
    else registry.remove<T>(entity);
}

template<class Owner, class Variant> void ApplyFields(Owner& target, const std::vector<Variant>& fields, bool forward) {
    for (const auto& field : fields) {
        std::visit([&](const auto& change) { target.*(change.member) = forward ? change.after : change.before; }, field);
    }
}

bool Replay(const Entry& entry, gameplay::World& world, RenderSettings& settings,
            gameplay::Entity& selection, bool forward) {
    if (!Preflight(entry, world, forward)) return false;
    auto& registry = world.Registry();
    VisitDeltas(entry, forward, [&](const Delta& delta) {
        // 先执行结构删除再按指定标识创建，精确保留 World 缓存的主实体句柄。
        for (const auto& change : delta.entities) {
            const bool sourceExists = forward ? change.beforeExists : change.afterExists;
            const bool targetExists = forward ? change.afterExists : change.beforeExists;
            if (sourceExists && !targetExists) registry.destroy(change.entity);
        }
        for (const auto& change : delta.entities) {
            const bool sourceExists = forward ? change.beforeExists : change.afterExists;
            const bool targetExists = forward ? change.afterExists : change.beforeExists;
            if (!sourceExists && targetExists) {
                const auto created = registry.create(change.entity);
                if (created != change.entity) {
                    registry.destroy(created);
                    throw std::logic_error("preflighted editor identity could not be restored");
                }
            }
        }
        for (const auto& change : delta.entities) {
            if (!(forward ? change.afterExists : change.beforeExists)) continue;
            ApplyComponent(registry, change.entity, change.name, forward);
            ApplyComponent(registry, change.entity, change.transform, forward);
            ApplyComponent(registry, change.entity, change.mesh, forward);
            ApplyComponent(registry, change.entity, change.camera, forward);
            ApplyComponent(registry, change.entity, change.light, forward);
            ApplyComponent(registry, change.entity, change.velocity, forward);
            ApplyComponent(registry, change.entity, change.script, forward);
            ApplyComponent(registry, change.entity, change.identity, forward);
        }
        // MaterialParameters 直接映射资源拥有的记录，不复制几何数据。
        for (const auto& material : delta.materials) ApplyFields(world.Materials()[material.index], material.fields, forward);
        for (const auto& script : delta.scripts) {
            const auto& value = forward ? script.after : script.before;
            auto* material = world.Material(value.materialIndex);
            material->ScriptSource() = value.source;
            material->ScriptPath() = value.path;
        }
        ApplyFields(settings, delta.settings, forward);
    });
    selection = forward ? entry.deltas.back().selectionAfter : entry.deltas.front().selectionBefore;
    if (!registry.valid(selection)) selection = entt::null;
    world.MarkDirty();
    settings.reset = true;
    return true;
}
}

struct EditorHistory::State {
    std::vector<Entry> entries; // 最多保存 128 次编辑交互，每项只包含实际变化。
    std::size_t cursor{}; // 此边界前的记录已应用，边界后的记录可重做。
    bool mergeBlocked{}; // 取消或回放操作将下一次编辑与之前同名交互分开。
};

EditorDocument CaptureEditorDocument(const gameplay::World& world, const RenderSettings& settings, gameplay::Entity selection) {
    EditorDocument document;
    document.settings = settings;
    document.selection = selection;
    const auto& registry = world.Registry();
    // 实体存储也包含组件视图会遗漏的无名称或无组件实体。
    const auto* entities = registry.storage<gameplay::Entity>();
    document.entities.reserve(entities->free_list());
    for (const auto [entity] : entities->each()) {
        document.entities.push_back({entity, CopyComponent<gameplay::Name>(registry, entity),
            CopyComponent<gameplay::Transform>(registry, entity), CopyComponent<gameplay::MeshRenderer>(registry, entity),
            CopyComponent<gameplay::Camera>(registry, entity), CopyComponent<gameplay::Light>(registry, entity),
            CopyComponent<gameplay::Velocity>(registry, entity), CopyComponent<gameplay::LuaScript>(registry, entity),
            CopyComponent<gameplay::ObjectIdentity>(registry, entity)});
    }
    std::sort(document.entities.begin(), document.entities.end(), [](const auto& a, const auto& b) {
        return entt::to_integral(a.entity) < entt::to_integral(b.entity);
    });
    document.materials.assign(world.Materials().begin(), world.Materials().end());
    for (std::size_t index = 0; index < world.Materials().size(); ++index) {
        const auto* material = world.Material(index);
        document.materialScripts.push_back({static_cast<std::uint32_t>(index), material->ScriptSource(), material->ScriptPath()});
    }
    return document;
}

EditorHistory::EditorHistory() : state_(std::make_unique<State>()) {}
EditorHistory::~EditorHistory() = default;
EditorHistory::EditorHistory(EditorHistory&&) noexcept = default;
EditorHistory& EditorHistory::operator=(EditorHistory&&) noexcept = default;

bool EditorHistory::Record(const EditorDocument& before, const EditorDocument& after, std::string label, bool merge) {
    auto delta = Difference(before, after);
    if (Empty(delta)) return false;
    if (!state_) state_ = std::make_unique<State>();
    // 无变化时提前返回，保留重做分支和当前交互边界。
    const bool extend = merge && !state_->mergeBlocked && state_->cursor == state_->entries.size() && state_->cursor != 0 &&
                        state_->entries.back().label == label;
    state_->entries.erase(state_->entries.begin() + static_cast<std::ptrdiff_t>(state_->cursor), state_->entries.end());
    if (extend) {
        Extend(state_->entries.back(), std::move(delta));
        // 恢复到交互起始状态时，不应额外占用一次场景撤销。
        state_->mergeBlocked = state_->entries.back().deltas.empty();
        if (state_->mergeBlocked) state_->entries.pop_back();
    } else {
        state_->entries.push_back({std::move(label), {std::move(delta)}});
        state_->mergeBlocked = false;
    }
    if (state_->entries.size() > 128) state_->entries.erase(state_->entries.begin());
    state_->cursor = state_->entries.size();
    return true;
}

bool EditorHistory::CanUndo() const { return state_ && state_->cursor != 0; }
bool EditorHistory::CanRedo() const { return state_ && state_->cursor < state_->entries.size(); }
std::string_view EditorHistory::UndoLabel() const { return CanUndo() ? state_->entries[state_->cursor - 1].label : std::string_view{}; }
std::string_view EditorHistory::RedoLabel() const { return CanRedo() ? state_->entries[state_->cursor].label : std::string_view{}; }

bool EditorHistory::Undo(gameplay::World& world, RenderSettings& settings, gameplay::Entity& selection) {
    if (!CanUndo() || !Replay(state_->entries[state_->cursor - 1], world, settings, selection, false)) return false;
    --state_->cursor;
    state_->mergeBlocked = true;
    return true;
}

bool EditorHistory::Redo(gameplay::World& world, RenderSettings& settings, gameplay::Entity& selection) {
    if (!CanRedo() || !Replay(state_->entries[state_->cursor], world, settings, selection, true)) return false;
    ++state_->cursor;
    state_->mergeBlocked = true;
    return true;
}

void EditorHistory::Clear() {
    if (!state_) return;
    state_->entries.clear();
    state_->cursor = 0;
    state_->mergeBlocked = true;
}
}
