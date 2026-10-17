#pragma once

#include <entt/meta/factory.hpp>
#include <entt/meta/resolve.hpp>
#include <entt/meta/container.hpp>
#include <entt/core/hashed_string.hpp>
#include <algorithm>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine::reflection {
enum class TypeKind {
    Primitive, // 数值、布尔值和字符串等基础类型。
    Enumeration, // 具有命名常量的强类型枚举。
    Struct, // 以字段和属性组织数据的值结构体。
    Class, // 以构造函数、继承和成员函数组织行为的类。
};

struct Symbol {
    std::string name; // 脚本与工具共同使用的稳定成员名称。
    std::string description; // 中文用途说明，供运行时类型发现使用。
    std::vector<bool> mutableArguments; // 标记需要回写的非常量左值引用参数。
};

struct TypeInfo {
    Symbol symbol; // 类型的公开名称与用途说明。
    TypeKind kind{}; // 区分基础类型、枚举、结构体与类。
    std::vector<std::vector<entt::meta_type>> constructors; // 显式开放的构造函数参数签名。
    std::map<entt::id_type, std::pair<std::string, bool>> members; // 检测哈希碰撞及字段、函数重名。
    entt::meta_any (*copy)(const entt::meta_any&){}; // 可复制类型的拥有型快照工厂，不要求默认构造。
};

class Registry;
template<class Type> class TypeBuilder;

// 每个注册表拥有独立上下文；冻结后可以查询和调用，但不再接收新元数据。
class Registry {
public:
    Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    template<class T>
    TypeBuilder<T> Type(std::string name, TypeKind kind = TypeKind::Struct, std::string description = {});
    template<auto Candidate>
    void Function(std::string name, std::string description = {});
    entt::meta_type Resolve(std::string_view name) const;
    entt::meta_type Globals() const;
    std::vector<entt::meta_type> Types() const;
    std::string Name(entt::meta_type type) const;
    entt::meta_any Construct(std::string_view name, std::span<entt::meta_any> arguments = {}) const;
    entt::meta_any Invoke(entt::meta_any& instance, std::string_view name, std::span<entt::meta_any> arguments = {}) const;
    entt::meta_any Call(std::string_view name, std::span<entt::meta_any> arguments = {}) const;
    const entt::meta_ctx& Context() const { return context_; }
    void Freeze() { frozen_ = true; }
    bool Frozen() const { return frozen_; }
    void RequireMutable() const;
    static entt::id_type Id(std::string_view name) { return entt::hashed_string::value(name.data(), name.size()); }
    static const TypeInfo& Info(entt::meta_type type);
    static entt::meta_any Copy(const entt::meta_any& value);
    static std::string_view KindName(TypeKind kind);

private:
    struct GlobalScope {}; // 全局函数在元数据上下文中的内部命名空间。
    template<class> friend class TypeBuilder;
    entt::meta_ctx context_; // 先于所有引用它的元数据创建，最后销毁。
    std::map<std::string, entt::id_type, std::less<>> names_; // 按公开名称排序的类型索引。
    bool frozen_{}; // 首次编译后禁止修改，避免运行中改变调用签名。
};

// 注册器只接受编译期成员指针，不依赖字节偏移、函数地址转换或布局猜测。
template<class Type>
class TypeBuilder {
public:
    template<class... Arguments> TypeBuilder& Constructor() {
        registry_.RequireMutable();
        auto& info = const_cast<TypeInfo&>(Registry::Info(Meta()));
        std::vector<entt::meta_type> signature{entt::resolve<Arguments>(registry_.context_)...};
        if (std::find(info.constructors.begin(), info.constructors.end(), signature) != info.constructors.end())
            throw std::invalid_argument("重复的反射构造函数");
        Factory().template ctor<Arguments...>();
        info.constructors.push_back(std::move(signature));
        return *this;
    }
    template<class Parent> TypeBuilder& Base() {
        registry_.RequireMutable();
        if (!static_cast<TypeInfo*>(entt::resolve<Parent>(registry_.context_).custom()))
            throw std::invalid_argument("反射基类必须先注册");
        Factory().template base<Parent>();
        return *this;
    }
    template<auto Member> TypeBuilder& Field(std::string name, std::string description = {}) {
        MemberName(name, false);
        Factory().template data<Member>(Registry::Id(name))
            .template custom<Symbol>(Symbol{std::move(name), std::move(description)});
        return *this;
    }
    template<auto Member> TypeBuilder& ReadOnly(std::string name, std::string description = {}) {
        MemberName(name, false);
        Factory().template data<nullptr, &ReadMember<Member>>(Registry::Id(name))
            .template custom<Symbol>(Symbol{std::move(name), std::move(description)});
        return *this;
    }
    template<auto Setter, auto Getter> TypeBuilder& Property(std::string name, std::string description = {}) {
        MemberName(name, false);
        Factory().template data<Setter, Getter>(Registry::Id(name))
            .template custom<Symbol>(Symbol{std::move(name), std::move(description)});
        return *this;
    }
    template<auto Function> TypeBuilder& Method(std::string name, std::string description = {}) {
        MemberName(name, true);
        using Arguments = typename entt::meta_function_helper_t<Type, decltype(Function)>::args_type;
        Factory().template func<Function>(Registry::Id(name))
            .template custom<Symbol>(Symbol{std::move(name), std::move(description), Mutable(Arguments{})});
        return *this;
    }
    template<class Target> TypeBuilder& Conversion() {
        registry_.RequireMutable();
        Factory().template conv<Target>();
        return *this;
    }
    template<auto Constant> TypeBuilder& Value(std::string name, std::string description = {}) {
        MemberName(name, false);
        Factory().template data<Constant>(Registry::Id(name))
            .template custom<Symbol>(Symbol{std::move(name), std::move(description)});
        return *this;
    }
    entt::meta_type Meta() const { return registry_.Resolve(name_); }

private:
    friend class Registry;
    TypeBuilder(Registry& registry, std::string name) : registry_(registry), name_(std::move(name)) {}
    auto Factory() { return entt::meta_factory<Type>{registry_.context_}; }
    template<class... Arguments> static std::vector<bool> Mutable(entt::type_list<Arguments...>) {
        return {(std::is_lvalue_reference_v<Arguments> && !std::is_const_v<std::remove_reference_t<Arguments>>)...};
    }
    template<auto Member, class Owner = Type> static auto ReadMember(const Owner& instance) { return instance.*Member; }
    void MemberName(const std::string& name, bool function) {
        registry_.RequireMutable();
        if (name.empty() || name.find('\0') != std::string::npos) throw std::invalid_argument("反射成员名称无效");
        auto& members = const_cast<TypeInfo&>(Registry::Info(Meta())).members;
        const auto [found, inserted] = members.try_emplace(Registry::Id(name), name, function);
        if (!inserted && (found->second.first != name || !function || !found->second.second))
            throw std::invalid_argument("重复的反射字段、成员重名或哈希碰撞：" + name);
    }
    Registry& registry_; // 注册时借用上下文，不能超过注册表的生命周期。
    std::string name_; // 所属类型的稳定公开名称。
};

template<class T>
TypeBuilder<T> Registry::Type(std::string name, TypeKind kind, std::string description) {
    RequireMutable();
    if (name.empty() || name.find('\0') != std::string::npos || name.front() == '$')
        throw std::invalid_argument("反射类型名称无效");
    const auto id = Id(name);
    if (names_.contains(name) || entt::resolve(context_, id) || static_cast<TypeInfo*>(entt::resolve<T>(context_).custom()))
        throw std::invalid_argument("重复的反射类型名称、C++ 类型或哈希碰撞：" + name);
    TypeInfo info{{name, std::move(description), {}}, kind, {}, {}, nullptr};
    if constexpr (std::is_copy_constructible_v<T>) {
        info.copy = +[](const entt::meta_any& value) {
            return entt::meta_any{value.context(), value.cast<const T&>()};
        };
    }
    entt::meta_factory<T>{context_}.type(id).template custom<TypeInfo>(std::move(info));
    names_.emplace(name, id);
    return {*this, std::move(name)};
}

template<auto Candidate>
void Registry::Function(std::string name, std::string description) {
    TypeBuilder<GlobalScope>{*this, "$globals"}.template Method<Candidate>(std::move(name), std::move(description));
}
}
