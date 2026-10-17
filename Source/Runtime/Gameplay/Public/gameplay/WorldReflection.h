#pragma once
#include "gameplay/World.h"
#include "reflection/Registry.h"
#include <functional>

namespace gameplay {
struct ComponentBinding {
    entt::meta_type type; // 与注册表同上下文的组件类型。
    bool writable{true}; // 资源身份等只读组件不能通过快照替换。
    std::function<bool(const World&, Entity)> has; // 每次访问重新检查组件存在性。
    std::function<entt::meta_any(const World&, Entity)> read; // 返回拥有数据的组件快照。
    std::function<entt::meta_any(const entt::meta_any&)> copy; // 暂存写入不借用 Lua 对象内存。
    std::function<void(const World&, Entity, const entt::meta_any&)> validate; // 提交前复核业务范围与不可变字段。
    std::function<bool(World&, Entity, entt::meta_any&)> commit; // 无异常交换，保证多组件提交不留下半成品。
};

using ComponentWrites = std::map<std::pair<Entity, entt::id_type>, entt::meta_any>;

// 类型反射与 ECS 组件适配分离，自定义组件无需增加 Lua 包装函数。
class WorldReflection {
public:
    WorldReflection();
    engine::reflection::Registry& Types() { return types_; }
    const engine::reflection::Registry& Types() const { return types_; }
    const ComponentBinding& Component(std::string_view name) const;
    const ComponentBinding& Component(entt::id_type id) const;
    const auto& Components() const { return components_; }
    template<class Type>
    void RegisterComponent(std::string_view name,
        std::function<void(const Type&, const Type&, const World&)> validate = {}, bool writable = true) {
        static_assert(std::is_nothrow_swappable_v<Type>, "组件事务提交要求无异常交换");
        types_.RequireMutable();
        const auto type = types_.Resolve(name);
        if (!type || type.info() != entt::type_id<Type>()) throw std::invalid_argument("组件类型尚未注册");
        if (components_.contains(type.id())) throw std::invalid_argument("组件适配重复注册");
        ComponentBinding binding;
        binding.type = type; binding.writable = writable;
        binding.has = [](const World& world, Entity entity) { return world.Registry().all_of<Type>(entity); };
        binding.read = [this](const World& world, Entity entity) {
            return entt::meta_any{types_.Context(), world.Registry().get<Type>(entity)};
        };
        binding.copy = [this](const entt::meta_any& value) {
            return entt::meta_any{types_.Context(), value.cast<const Type&>()};
        };
        binding.validate = [validate](const World& world, Entity entity, const entt::meta_any& value) {
            if (!world.Registry().valid(entity) || !world.Registry().all_of<Type>(entity))
                throw std::invalid_argument("组件目标实体已失效或组件已移除");
            const auto* candidate = value.try_cast<Type>();
            if (!candidate) throw std::invalid_argument("组件快照类型不匹配");
            if (validate) validate(*candidate, world.Registry().get<Type>(entity), world);
        };
        binding.commit = [](World& world, Entity entity, entt::meta_any& value) {
            auto& current = world.Registry().get<Type>(entity);
            auto& candidate = value.cast<Type&>();
            // Transform 读取不产生写入；相同位姿重复设置也不重置渲染历史。
            if constexpr (std::is_same_v<Type, Transform>) {
                const auto same = [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
                if (same(current.position, candidate.position) && same(current.rotationDegrees, candidate.rotationDegrees) &&
                    same(current.scale, candidate.scale)) return false;
            } else if constexpr (requires { { static_cast<bool>(current == candidate) } noexcept; }) {
                // 用户组件的比较也可能抛异常，只有无异常比较才参与提交阶段的去重。
                if (static_cast<bool>(current == candidate)) return false;
            }
            using std::swap;
            swap(current, candidate);
            return true;
        };
        components_.emplace(type.id(), std::move(binding));
    }
    entt::meta_any Read(const World&, Entity, std::string_view component, const ComponentWrites&) const;
    void Stage(const World&, Entity, std::string_view component, const entt::meta_any&, ComponentWrites&) const;
    void Commit(World&, ComponentWrites&) const;

private:
    engine::reflection::Registry types_; // 比组件适配和虚拟机值更早创建、更晚销毁。
    std::map<entt::id_type, ComponentBinding> components_; // 只保存类型操作，不保存 ECS 组件指针。
};
}
