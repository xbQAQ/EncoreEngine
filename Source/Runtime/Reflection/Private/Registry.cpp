#include "reflection/Registry.h"

namespace engine::reflection {
Registry::Registry() {
    // 基础类型使用固定公开名称，C++ 别名共享同一份类型元数据。
    entt::meta_factory<GlobalScope>{context_}.type(Id("$globals"))
        .custom<TypeInfo>(TypeInfo{{"$globals", "全局函数"}, TypeKind::Class, {}, {}});
    Type<void>("void", TypeKind::Primitive);
    Type<bool>("bool", TypeKind::Primitive);
    Type<char>("char", TypeKind::Primitive);
    Type<signed char>("int8", TypeKind::Primitive);
    Type<unsigned char>("uint8", TypeKind::Primitive);
    Type<short>("int16", TypeKind::Primitive);
    Type<unsigned short>("uint16", TypeKind::Primitive);
    Type<int>("int", TypeKind::Primitive);
    Type<unsigned int>("uint", TypeKind::Primitive);
    Type<long>("long", TypeKind::Primitive);
    Type<unsigned long>("ulong", TypeKind::Primitive);
    Type<long long>("int64", TypeKind::Primitive);
    Type<unsigned long long>("uint64", TypeKind::Primitive);
    Type<float>("float", TypeKind::Primitive);
    Type<double>("double", TypeKind::Primitive);
    Type<std::string>("string", TypeKind::Primitive);
}

void Registry::RequireMutable() const {
    if (frozen_) throw std::logic_error("反射注册表已冻结，请在首次编译脚本之前完成注册");
}

// 公开名称独立于 C++ 类型名，避免编译器修饰名成为脚本的长期接口。
entt::meta_type Registry::Resolve(std::string_view name) const {
    if (name == "$globals") return Globals();
    const auto found = names_.find(name);
    return found == names_.end() ? entt::meta_type{} : entt::resolve(context_, found->second);
}

entt::meta_type Registry::Globals() const { return entt::resolve<GlobalScope>(context_); }

std::vector<entt::meta_type> Registry::Types() const {
    std::vector<entt::meta_type> types;
    for (const auto& [name, id] : names_) types.push_back(entt::resolve(context_, id));
    return types;
}

std::string Registry::Name(entt::meta_type type) const {
    if (const auto* info = static_cast<TypeInfo*>(type.custom())) return info->symbol.name;
    return type ? std::string(type.info().name()) : std::string{};
}

const TypeInfo& Registry::Info(entt::meta_type type) {
    const auto* info = static_cast<TypeInfo*>(type.custom());
    if (!info) throw std::invalid_argument("未注册的反射类型");
    return *info;
}

// 显式复制工厂支持不能默认构造的元素，并将借用值转换为拥有型快照。
entt::meta_any Registry::Copy(const entt::meta_any& value) {
    const auto& info = Info(value.type());
    if (!info.copy) throw std::invalid_argument("该反射类型不可复制：" + info.symbol.name);
    return info.copy(value);
}

std::string_view Registry::KindName(TypeKind kind) {
    switch (kind) {
    case TypeKind::Primitive: return "primitive";
    case TypeKind::Enumeration: return "enum";
    case TypeKind::Struct: return "struct";
    case TypeKind::Class: return "class";
    }
    throw std::invalid_argument("无效的反射类型类别");
}

entt::meta_any Registry::Construct(std::string_view name, std::span<entt::meta_any> arguments) const {
    const auto type = Resolve(name);
    if (!type) throw std::invalid_argument("未知的反射类型：" + std::string(name));
    const auto& signatures = Info(type).constructors;
    // 只开放显式登记的构造函数，避免默认构造能力意外暴露宿主服务。
    bool allowed = false;
    for (const auto& signature : signatures) {
        if (signature.size() != arguments.size()) continue;
        bool matches = true;
        for (std::size_t i = 0; i < signature.size(); ++i)
            matches &= arguments[i].type().can_cast(signature[i]) || arguments[i].type().can_convert(signature[i]);
        allowed |= matches;
    }
    if (!allowed) throw std::invalid_argument("没有匹配的公开反射构造函数：" + std::string(name));
    auto result = type.construct(arguments.data(), arguments.size());
    if (!result) throw std::invalid_argument("反射构造参数不匹配：" + std::string(name));
    return result;
}

// 上下文不能混用；相同 C++ 类型在另一个注册表中可能开放不同能力。
entt::meta_any Registry::Invoke(entt::meta_any& instance, std::string_view name, std::span<entt::meta_any> arguments) const {
    if (&instance.context() != &context_) throw std::invalid_argument("反射对象属于其他注册表");
    auto result = instance.type().invoke(Id(name), instance, arguments.data(), arguments.size());
    if (!result) throw std::invalid_argument("反射成员函数或参数不匹配：" + std::string(name));
    return result;
}

entt::meta_any Registry::Call(std::string_view name, std::span<entt::meta_any> arguments) const {
    auto result = Globals().invoke(Id(name), {}, arguments.data(), arguments.size());
    if (!result) throw std::invalid_argument("反射全局函数或参数不匹配：" + std::string(name));
    return result;
}
}
