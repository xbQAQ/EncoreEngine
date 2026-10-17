#include "reflection/Lua.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <variant>

namespace engine::reflection {
namespace {
// 元表使用引擎专属命名空间，避免同一 Lua 状态中的其他绑定误认反射值。
constexpr const char* ObjectMetatable = "Encore.Reflection.Value";
char registryKey;
using Step = std::variant<std::string, std::size_t, entt::meta_any>;

struct Object {
    std::shared_ptr<entt::meta_any> root; // 拥有根值，子属性不借用 Lua 栈或 ECS 地址。
    std::vector<Step> path; // 每次访问重新解析路径，容器重新分配后仍可安全检查。
};

// 注册表由宿主拥有，Lua 只在私有 registry 中保存非拥有指针。
const Registry& Context(lua_State* lua) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &registryKey);
    auto* registry = static_cast<const Registry*>(lua_touserdata(lua, -1));
    lua_pop(lua, 1);
    if (!registry) throw std::invalid_argument("Lua 反射上下文不可用");
    return *registry;
}

// 禁止 Lua 将数字隐式转换为成员名称，保持重载和属性访问的类型规则一致。
std::string String(lua_State* lua, int index) {
    if (lua_type(lua, index) != LUA_TSTRING) throw std::invalid_argument("反射接口要求字符串参数");
    std::size_t size{};
    const char* value = lua_tolstring(lua, index, &size);
    return {value, size};
}

// 仅接受本桥接层创建的 userdata，避免把其他原生对象解释成反射值。
Object& Box(lua_State* lua, int index) {
    auto* object = static_cast<Object*>(luaL_testudata(lua, index, ObjectMetatable));
    if (!object || !object->root) throw std::invalid_argument("反射接口要求有效对象");
    return *object;
}

// 路径解析返回拥有型子值，后续容器变化不会使已解析值持有悬空地址。
entt::meta_any Child(entt::meta_any& parent, const Step& step) {
    if (const auto* name = std::get_if<std::string>(&step)) {
        const auto field = parent.type().data(Registry::Id(*name));
        if (!field) throw std::invalid_argument("未知反射字段：" + *name);
        return field.get(parent);
    }
    if (const auto* index = std::get_if<std::size_t>(&step)) {
        auto sequence = parent.as_sequence_container();
        if (!sequence || *index >= sequence.size()) throw std::invalid_argument("反射序列下标越界");
        // 元素先复制成值，防止后续调用改变容器后留下悬空元素地址。
        auto element = sequence[*index];
        return Registry::Copy(element);
    }
    auto associative = parent.as_associative_container();
    if (!associative || !associative.mapped_type()) throw std::invalid_argument("反射对象不是键值容器");
    auto found = associative.find(std::get<entt::meta_any>(step));
    if (found == associative.end()) throw std::invalid_argument("反射容器中没有该键");
    auto element = (*found).second;
    return Registry::Copy(element);
}

// 字段必须经过注册的 setter；容器只替换现有序列位置或插入合法键值。
void StoreChild(entt::meta_any& parent, const Step& step, entt::meta_any value) {
    bool stored = false;
    if (const auto* name = std::get_if<std::string>(&step)) {
        const auto field = parent.type().data(Registry::Id(*name));
        stored = field && !field.is_const() && field.set(parent, std::move(value));
    } else if (const auto* index = std::get_if<std::size_t>(&step)) {
        auto sequence = parent.as_sequence_container();
        stored = sequence && *index < sequence.size() && sequence[*index].assign(value);
    } else {
        auto map = parent.as_associative_container();
        if (map && map.mapped_type()) {
            auto key = std::get<entt::meta_any>(step);
            auto found = map.find(key);
            stored = found == map.end() ? map.insert(std::move(key), std::move(value)) : (*found).second.assign(value);
        }
    }
    if (!stored) throw std::invalid_argument("反射字段只读，或写入类型不匹配");
}

// 每次从仍然存活的根值重新解析路径，让旧代理观察到父值的最新状态。
entt::meta_any Resolve(const Object& object) {
    auto current = object.root->as_ref();
    for (const auto& step : object.path) current = Child(current, step);
    if (!current) throw std::invalid_argument("反射对象的属性已失效");
    return current;
}

// 嵌套编辑逐层经过 setter 回写，让只读属性和访问器校验始终生效。
void StorePath(entt::meta_any& parent, const std::vector<Step>& path, std::size_t depth, entt::meta_any value) {
    if (depth + 1 == path.size()) { StoreChild(parent, path[depth], std::move(value)); return; }
    auto child = Child(parent, path[depth]);
    StorePath(child, path, depth + 1, std::move(value));
    StoreChild(parent, path[depth], std::move(child));
}

void Save(Object& object, entt::meta_any value) {
    if (!object.path.empty()) {
        auto root = object.root->as_ref();
        StorePath(root, object.path, 0, std::move(value));
    }
}

// 浮点转整数使用二次幂的开区间上界，避免最大整数舍入后触发未定义转换。
template<class Type>
std::optional<entt::meta_any> Number(lua_State* lua, int index, entt::meta_type expected) {
    if (expected.info() != entt::type_id<Type>()) return std::nullopt;
    if (lua_type(lua, index) != LUA_TNUMBER) throw std::invalid_argument("反射数值参数类型不匹配");
    const auto& context = Context(lua).Context();
    if constexpr (std::is_integral_v<Type>) {
        if (lua_isinteger(lua, index)) {
            const auto value = lua_tointeger(lua, index);
            using Range = std::conditional_t<std::is_same_v<Type, char>,
                std::conditional_t<std::is_signed_v<char>, signed char, unsigned char>, Type>;
            if (!std::in_range<Range>(value)) throw std::invalid_argument("反射整数超出目标范围");
            return entt::meta_any{context, static_cast<Type>(value)};
        }
        const double value = lua_tonumber(lua, index);
        const double bound = std::ldexp(1.0, std::numeric_limits<Type>::digits);
        const double lower = std::is_signed_v<Type> ? -bound : 0;
        if (!std::isfinite(value) || std::trunc(value) != value || value < lower || value >= bound)
            throw std::invalid_argument("反射整数必须是范围内的有限整数");
        return entt::meta_any{context, static_cast<Type>(value)};
    } else {
        const double value = lua_tonumber(lua, index);
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<Type>::max())
            throw std::invalid_argument("反射浮点数超出有限值范围");
        return entt::meta_any{context, static_cast<Type>(value)};
    }
}

// 先区分 Lua 整数和浮点数，再优先精确对象类型；同分候选由调用方拒绝。
int Score(lua_State* lua, int index, entt::meta_type expected) {
    if (expected.is_integral() && expected.info() != entt::type_id<bool>())
        return lua_isinteger(lua, index) ? (expected.info() == entt::type_id<lua_Integer>() ? 0 : 1) : 3;
    if (expected.is_arithmetic() && expected.info() != entt::type_id<bool>())
        return (lua_isinteger(lua, index) ? 2 : 0) + (expected.info() == entt::type_id<double>() ? 0 : 1);
    if (luaL_testudata(lua, index, ObjectMetatable)) return Resolve(Box(lua, index)).type() == expected ? 0 : 1;
    return 0;
}

// 别名按根对象和路径判断，与 Lua 是否复用同一个代理 userdata 无关。
bool SameLocation(const Object& left, const Object& right) {
    return left.root == right.root && left.path == right.path;
}

bool OverlappingLocations(const Object& left, const Object& right) {
    const auto count = std::min(left.path.size(), right.path.size());
    return left.root == right.root && left.path.size() != right.path.size() &&
        std::equal(left.path.begin(), left.path.begin() + count, right.path.begin());
}

std::vector<entt::meta_any> Arguments(lua_State* lua, int first, const std::vector<entt::meta_type>& signature,
    const Object* receiver = nullptr, entt::meta_any* instance = nullptr) {
    if (static_cast<std::size_t>(std::max(0, lua_gettop(lua) - first + 1)) != signature.size())
        throw std::invalid_argument("反射调用的参数数量不匹配");
    std::vector<entt::meta_any> values;
    // 预留空间使前面参数的拥有型值保持地址稳定，重复路径随后只引用这份值。
    values.reserve(signature.size());
    for (std::size_t i = 0; i < signature.size(); ++i) {
        const int index = first + static_cast<int>(i);
        auto value = ReadLua(lua, index, signature[i]);
        if (luaL_testudata(lua, index, ObjectMetatable)) {
            const auto& object = Box(lua, index);
            if (receiver && SameLocation(object, *receiver) && value.type() == instance->type()) {
                value = instance->as_ref();
            } else for (std::size_t previous = 0; previous < i; ++previous) {
                const int previousIndex = first + static_cast<int>(previous);
                if (luaL_testudata(lua, previousIndex, ObjectMetatable) &&
                    SameLocation(object, Box(lua, previousIndex)) && value.type() == values[previous].type()) {
                    value = values[previous].as_ref();
                    break;
                }
            }
        }
        values.push_back(std::move(value));
    }
    return values;
}

// 所有签名先经过严格转换，只有唯一的最优候选才能进入原生调用。
template<class Candidate>
std::pair<Candidate, std::vector<entt::meta_any>> Select(lua_State* lua, int first,
    const std::vector<std::pair<Candidate, std::vector<entt::meta_type>>>& candidates,
    const Object* receiver = nullptr, entt::meta_any* instance = nullptr) {
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;
    Candidate best{};
    std::vector<entt::meta_any> arguments;
    for (const auto& [candidate, signature] : candidates) {
        try {
            // 静态函数虽然可以使用实例语法调用，但没有参与参数别名的接收对象。
            const Object* activeReceiver = receiver;
            if constexpr (std::is_same_v<Candidate, entt::meta_func>) {
                if (candidate.is_static()) activeReceiver = nullptr;
            }
            auto current = Arguments(lua, first, signature, activeReceiver, instance);
            int score = 0;
            for (std::size_t i = 0; i < signature.size(); ++i) score += Score(lua, first + static_cast<int>(i), signature[i]);
            if (score < bestScore) { best = candidate; arguments = std::move(current); bestScore = score; ambiguous = false; }
            else if (score == bestScore) ambiguous = true;
        } catch (const std::invalid_argument&) {}
    }
    if (bestScore == std::numeric_limits<int>::max()) throw std::invalid_argument("没有匹配的反射调用签名");
    if (ambiguous) throw std::invalid_argument("反射调用存在重载歧义");
    return {best, std::move(arguments)};
}

struct Invocation {
    entt::meta_any result; // 调用结果始终按值拥有，不能保留函数栈地址。
    std::vector<entt::meta_any> arguments; // 引用参数在回写完成前保持存活。
    entt::meta_func function; // 已选中的唯一函数重载及常量限定。
    int first{}; // 第一个 Lua 实参的位置，用于回写嵌套对象。
};

Invocation Invoke(lua_State* lua, entt::meta_type type, entt::meta_any& instance, const std::string& name, int first,
    const Object* receiver = nullptr) {
    std::vector<std::pair<entt::meta_func, std::vector<entt::meta_type>>> candidates;
    for (auto function = type.func(Registry::Id(name)); function; function = function.next()) {
        if (!instance && !function.is_static()) continue;
        std::vector<entt::meta_type> signature;
        for (std::size_t i = 0; i < function.arity(); ++i) signature.push_back(function.arg(i));
        candidates.emplace_back(function, std::move(signature));
    }
    auto [function, arguments] = Select(lua, first, candidates, receiver, &instance);
    // 属性采用复制及 setter 回写，重叠的父子路径不能同时充当可写引用。
    const auto* symbol = static_cast<Symbol*>(function.custom());
    if (symbol) for (std::size_t i = 0; i < arguments.size(); ++i) {
        const int index = first + static_cast<int>(i);
        if (!luaL_testudata(lua, index, ObjectMetatable)) continue;
        const auto& object = Box(lua, index);
        const bool mutableArgument = symbol->mutableArguments[i];
        if (receiver && !function.is_static() && (mutableArgument || !function.is_const()) &&
            OverlappingLocations(object, *receiver))
            throw std::invalid_argument("反射调用不能同时修改重叠的父子属性路径");
        for (std::size_t previous = 0; previous < i; ++previous) {
            const int previousIndex = first + static_cast<int>(previous);
            if ((mutableArgument || symbol->mutableArguments[previous]) &&
                luaL_testudata(lua, previousIndex, ObjectMetatable) &&
                OverlappingLocations(object, Box(lua, previousIndex)))
                throw std::invalid_argument("反射调用不能同时修改重叠的父子属性路径");
        }
    }
    auto result = function.invoke(instance, arguments.data(), arguments.size());
    if (!result) throw std::invalid_argument("反射函数调用失败：" + name);
    return {std::move(result), std::move(arguments), function, first};
}

int Finish(lua_State* lua, Invocation call) {
    const auto* symbol = static_cast<Symbol*>(call.function.custom());
    // 参数先经过完整 C++ 调用，再回写其嵌套路径；标量输出追加到返回值。
    if (symbol) for (std::size_t i = 0; i < symbol->mutableArguments.size(); ++i) {
        if (symbol->mutableArguments[i] && luaL_testudata(lua, call.first + static_cast<int>(i), ObjectMetatable))
            Save(Box(lua, call.first + static_cast<int>(i)), call.arguments[i].as_ref());
    }
    int count = PushLua(lua, std::move(call.result));
    if (symbol) for (std::size_t i = 0; i < symbol->mutableArguments.size(); ++i) {
        if (symbol->mutableArguments[i] && !luaL_testudata(lua, call.first + static_cast<int>(i), ObjectMetatable))
            count += PushLua(lua, std::move(call.arguments[i]));
    }
    return count;
}

template<int(*Function)(lua_State*)> int Safe(lua_State* lua) { return GuardLua(lua, Function); }
// 常量查询不触发 setter，避免对只读属性执行无意义的回写。
int Method(lua_State* lua) {
    auto& object = Box(lua, 1);
    auto instance = Resolve(object);
    auto result = Invoke(lua, instance.type(), instance, String(lua, lua_upvalueindex(1)), 2, &object);
    if (!result.function.is_const() && !result.function.is_static()) Save(object, instance.as_ref());
    return Finish(lua, std::move(result));
}

// Lua 序列使用一基索引，键值容器的键则按其反射类型严格转换。
Step IndexStep(lua_State* lua, entt::meta_any& instance, int index) {
    if (auto sequence = instance.as_sequence_container()) {
        if (!lua_isinteger(lua, index) || lua_tointeger(lua, index) <= 0 ||
            static_cast<lua_Unsigned>(lua_tointeger(lua, index)) > sequence.size())
            throw std::invalid_argument("反射序列下标必须是有效的一基整数");
        return static_cast<std::size_t>(lua_tointeger(lua, index) - 1);
    }
    if (auto map = instance.as_associative_container()) return ReadLua(lua, index, map.key_type());
    return String(lua, index);
}

// 复合子值替换为根值路径代理，父代理释放不会终止子值的生命周期。
int Index(lua_State* lua) {
    auto& object = Box(lua, 1);
    auto instance = Resolve(object);
    auto step = IndexStep(lua, instance, 2);
    if (const auto* name = std::get_if<std::string>(&step); name && !instance.type().data(Registry::Id(*name))) {
        if (!instance.type().func(Registry::Id(*name))) throw std::invalid_argument("未知反射成员：" + *name);
        lua_pushlstring(lua, name->data(), name->size());
        lua_pushcclosure(lua, Safe<Method>, 1);
        return 1;
    }
    auto child = Child(instance, step);
    const int count = PushLua(lua, std::move(child));
    if (auto* proxy = static_cast<Object*>(luaL_testudata(lua, -1, ObjectMetatable))) {
        proxy->root = object.root;
        proxy->path = object.path;
        proxy->path.push_back(std::move(step));
    }
    return count;
}

// 先校验目标字段与输入类型，再逐层回写，阻止只读父属性被嵌套编辑绕过。
int NewIndex(lua_State* lua) {
    auto& object = Box(lua, 1);
    auto instance = Resolve(object);
    auto step = IndexStep(lua, instance, 2);
    entt::meta_type expected;
    if (const auto* name = std::get_if<std::string>(&step)) {
        const auto field = instance.type().data(Registry::Id(*name));
        if (!field || field.is_const()) throw std::invalid_argument("未知或只读的反射字段：" + *name);
        expected = field.type();
    } else if (std::holds_alternative<std::size_t>(step)) expected = instance.as_sequence_container().value_type();
    else expected = instance.as_associative_container().mapped_type();
    StoreChild(instance, step, ReadLua(lua, 3, expected));
    Save(object, instance.as_ref());
    return 0;
}

// 静态字段无需构造实例，对象字段沿用代理路径以保持嵌套属性的读写规则。
int GetField(lua_State* lua) {
    if (lua_type(lua, 1) != LUA_TSTRING) return Index(lua);
    const auto field = Context(lua).Resolve(String(lua, 1)).data(Registry::Id(String(lua, 2)));
    if (!field || !field.is_static()) throw std::invalid_argument("类型名称只能用于读取已注册的静态字段");
    return PushLua(lua, field.get({}));
}
int SetField(lua_State* lua) {
    if (lua_type(lua, 1) != LUA_TSTRING) return NewIndex(lua);
    const auto field = Context(lua).Resolve(String(lua, 1)).data(Registry::Id(String(lua, 2)));
    if (!field || !field.is_static() || field.is_const()) throw std::invalid_argument("静态字段不存在或只读");
    if (!field.set({}, ReadLua(lua, 3, field.type()))) throw std::invalid_argument("静态字段写入失败");
    return 0;
}

// 容器规模和数值相等比较均从当前路径求值，不以代理地址作为值身份。
int Length(lua_State* lua) {
    auto instance = Resolve(Box(lua, 1));
    if (auto sequence = instance.as_sequence_container()) lua_pushinteger(lua, static_cast<lua_Integer>(sequence.size()));
    else if (auto map = instance.as_associative_container()) lua_pushinteger(lua, static_cast<lua_Integer>(map.size()));
    else throw std::invalid_argument("反射对象不是容器");
    return 1;
}

int Equal(lua_State* lua) {
    lua_pushboolean(lua, Resolve(Box(lua, 1)) == Resolve(Box(lua, 2)));
    return 1;
}
int ToString(lua_State* lua) {
    const auto name = Context(lua).Name(Resolve(Box(lua, 1)).type());
    lua_pushlstring(lua, name.data(), name.size());
    return 1;
}
int Destroy(lua_State* lua) { Box(lua, 1).~Object(); return 0; }

void TextField(lua_State* lua, const char* name, std::string_view value) {
    lua_pushlstring(lua, value.data(), value.size()); lua_setfield(lua, -2, name);
}
void BoolField(lua_State* lua, const char* name, bool value) { lua_pushboolean(lua, value); lua_setfield(lua, -2, name); }

// 同名重载分别描述，并递归列出继承能力，供脚本发现可用调用签名。
void Functions(lua_State* lua, entt::meta_type type, int& index) {
    for (const auto& [id, head] : type.func()) for (auto function = head; function; function = function.next()) {
        const auto* symbol = static_cast<Symbol*>(function.custom());
        if (!symbol) continue;
        lua_newtable(lua);
        TextField(lua, "name", symbol->name); TextField(lua, "description", symbol->description);
        TextField(lua, "return_type", Context(lua).Name(function.ret()));
        BoolField(lua, "is_const", function.is_const()); BoolField(lua, "is_static", function.is_static());
        lua_newtable(lua);
        for (std::size_t i = 0; i < function.arity(); ++i) {
            const auto name = Context(lua).Name(function.arg(i));
            lua_pushlstring(lua, name.data(), name.size()); lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(lua, -2, "parameters"); lua_rawseti(lua, -2, ++index);
    }
    for (const auto& [id, base] : type.base()) Functions(lua, base, index);
}

// 元数据仅返回公开名称与读写限制，不向脚本透露成员偏移或原生地址。
void Fields(lua_State* lua, entt::meta_type type, int& index) {
    for (const auto& [id, field] : type.data()) {
        const auto* symbol = static_cast<Symbol*>(field.custom());
        if (!symbol) continue;
        lua_newtable(lua);
        TextField(lua, "name", symbol->name); TextField(lua, "description", symbol->description);
        TextField(lua, "type", Context(lua).Name(field.type()));
        BoolField(lua, "read_only", field.is_const()); BoolField(lua, "is_static", field.is_static());
        lua_rawseti(lua, -2, ++index);
    }
    for (const auto& [id, base] : type.base()) Fields(lua, base, index);
}

// 查询产生普通 Lua 表副本，脚本修改描述不会改变宿主的冻结元数据。
int Describe(lua_State* lua) {
    const auto& registry = Context(lua);
    const auto type = lua_type(lua, 1) == LUA_TSTRING ? registry.Resolve(String(lua, 1)) : Resolve(Box(lua, 1)).type();
    const auto& info = Registry::Info(type);
    lua_newtable(lua);
    TextField(lua, "name", info.symbol.name); TextField(lua, "kind", Registry::KindName(info.kind));
    TextField(lua, "description", info.symbol.description);
    lua_pushinteger(lua, static_cast<lua_Integer>(type.size_of())); lua_setfield(lua, -2, "size");
    lua_newtable(lua); int index = 0; Fields(lua, type, index); lua_setfield(lua, -2, "fields");
    lua_newtable(lua); index = 0; Functions(lua, type, index); lua_setfield(lua, -2, "functions");
    lua_newtable(lua); index = 0;
    for (const auto& [id, base] : type.base()) {
        const auto name = registry.Name(base); lua_pushlstring(lua, name.data(), name.size()); lua_rawseti(lua, -2, ++index);
    }
    lua_setfield(lua, -2, "bases");
    lua_newtable(lua); index = 0;
    for (const auto& signature : info.constructors) {
        lua_newtable(lua); int argument = 0;
        for (const auto parameter : signature) {
            const auto name = registry.Name(parameter); lua_pushlstring(lua, name.data(), name.size()); lua_rawseti(lua, -2, ++argument);
        }
        lua_rawseti(lua, -2, ++index);
    }
    lua_setfield(lua, -2, "constructors");
    lua_newtable(lua);
    if (type.is_enum()) for (const auto& [id, field] : type.data()) {
        const auto* symbol = static_cast<Symbol*>(field.custom());
        if (symbol && field.is_static()) { PushLua(lua, field.get({})); lua_setfield(lua, -2, symbol->name.c_str()); }
    }
    lua_setfield(lua, -2, "values");
    return 1;
}

// 公开列表排除全局函数所用的内部命名空间。
int Types(lua_State* lua) {
    lua_newtable(lua); int index = 0;
    for (const auto type : Context(lua).Types()) {
        const auto name = Context(lua).Name(type); lua_pushlstring(lua, name.data(), name.size()); lua_rawseti(lua, -2, ++index);
    }
    return 1;
}
int GlobalFunctions(lua_State* lua) {
    lua_newtable(lua); int index = 0; Functions(lua, Context(lua).Globals(), index); return 1;
}
// 构造入口只接受显式公开的签名，普通类型查询不授予默认构造权限。
int Construct(lua_State* lua) {
    const auto name = String(lua, 1);
    const auto type = Context(lua).Resolve(name);
    const auto& info = Registry::Info(type);
    std::vector<std::pair<int, std::vector<entt::meta_type>>> candidates;
    int index = 0;
    for (const auto& signature : info.constructors) candidates.emplace_back(++index, signature);
    auto [selected, arguments] = Select(lua, 2, candidates);
    return PushLua(lua, Context(lua).Construct(name, arguments));
}
// 全局及静态调用不提供实例句柄，对象调用继续使用路径和引用回写规则。
int Call(lua_State* lua) {
    entt::meta_any instance{entt::meta_ctx_arg, Context(lua).Context()};
    return Finish(lua, Invoke(lua, Context(lua).Globals(), instance, String(lua, 1), 2));
}
int InvokeExplicit(lua_State* lua) {
    if (lua_type(lua, 1) == LUA_TSTRING) {
        entt::meta_any instance{entt::meta_ctx_arg, Context(lua).Context()};
        return Finish(lua, Invoke(lua, Context(lua).Resolve(String(lua, 1)), instance, String(lua, 2), 3));
    }
    auto& object = Box(lua, 1);
    auto instance = Resolve(object);
    auto result = Invoke(lua, instance.type(), instance, String(lua, 2), 3, &object);
    if (!result.function.is_const() && !result.function.is_static()) Save(object, instance.as_ref());
    return Finish(lua, std::move(result));
}
}

// 先退出 C++ 异常作用域，再让 Lua 以长跳转传播错误。
int GuardLua(lua_State* lua, int (*function)(lua_State*)) {
    try { return function(lua); }
    catch (const std::exception& error) { lua_pushstring(lua, error.what()); }
    catch (...) { lua_pushliteral(lua, "反射调用发生未知 C++ 异常"); }
    return lua_error(lua);
}

// 基础值按目标类型解析，对象仅允许同上下文中的继承或显式注册转换。
entt::meta_any ReadLua(lua_State* lua, int index, entt::meta_type expected) {
    const auto& registry = Context(lua);
    if (!expected) throw std::invalid_argument("未知反射参数类型");
    if (expected.info() == entt::type_id<std::string>()) return entt::meta_any{registry.Context(), String(lua, index)};
    if (expected.info() == entt::type_id<bool>()) {
        if (lua_type(lua, index) != LUA_TBOOLEAN) throw std::invalid_argument("反射布尔参数类型不匹配");
        return entt::meta_any{registry.Context(), lua_toboolean(lua, index) != 0};
    }
    // 整数转换逐类型检查范围，不使用会截断或溢出的隐式数值转换。
#define ENCORE_READ_NUMBER(T) if (auto value = Number<T>(lua, index, expected)) return std::move(*value)
    ENCORE_READ_NUMBER(char); ENCORE_READ_NUMBER(signed char); ENCORE_READ_NUMBER(unsigned char);
    ENCORE_READ_NUMBER(short); ENCORE_READ_NUMBER(unsigned short); ENCORE_READ_NUMBER(int); ENCORE_READ_NUMBER(unsigned int);
    ENCORE_READ_NUMBER(long); ENCORE_READ_NUMBER(unsigned long); ENCORE_READ_NUMBER(long long); ENCORE_READ_NUMBER(unsigned long long);
    ENCORE_READ_NUMBER(float); ENCORE_READ_NUMBER(double);
#undef ENCORE_READ_NUMBER
    auto value = Resolve(Box(lua, index));
    if (&value.context() != &registry.Context() || !value.allow_cast(expected))
        throw std::invalid_argument("反射对象类型不匹配：" + registry.Name(expected));
    return value;
}

// 标量使用 Lua 原生值，其余对象由 userdata 拥有，裸指针不跨越脚本边界。
int PushLua(lua_State* lua, entt::meta_any value) {
    if (!value) throw std::invalid_argument("无法向 Lua 返回无效反射值");
    if (value.type().info() == entt::type_id<void>()) return 0;
    if (value.type().is_pointer()) throw std::invalid_argument("Lua 反射不暴露原始 C++ 指针");
    if (const auto* text = value.try_cast<std::string>()) { lua_pushlstring(lua, text->data(), text->size()); return 1; }
    if (const auto* boolean = value.try_cast<bool>()) { lua_pushboolean(lua, *boolean); return 1; }
    // Lua 的有符号整数不能精确表达更大的 uint64，明确拒绝而非转成有损浮点数。
#define ENCORE_PUSH_INTEGER(T) if (const auto* number = value.try_cast<T>()) { \
    if (!std::in_range<lua_Integer>(+*number)) throw std::invalid_argument("反射整数超出 Lua 整数范围"); \
    lua_pushinteger(lua, static_cast<lua_Integer>(*number)); return 1; }
    ENCORE_PUSH_INTEGER(char); ENCORE_PUSH_INTEGER(signed char); ENCORE_PUSH_INTEGER(unsigned char);
    ENCORE_PUSH_INTEGER(short); ENCORE_PUSH_INTEGER(unsigned short); ENCORE_PUSH_INTEGER(int); ENCORE_PUSH_INTEGER(unsigned int);
    ENCORE_PUSH_INTEGER(long); ENCORE_PUSH_INTEGER(unsigned long); ENCORE_PUSH_INTEGER(long long); ENCORE_PUSH_INTEGER(unsigned long long);
#undef ENCORE_PUSH_INTEGER
    if (const auto* number = value.try_cast<float>()) { lua_pushnumber(lua, *number); return 1; }
    if (const auto* number = value.try_cast<double>()) { lua_pushnumber(lua, *number); return 1; }
    auto* storage = lua_newuserdatauv(lua, sizeof(Object), 0);
    new (storage) Object{std::make_shared<entt::meta_any>(std::move(value)), {}};
    luaL_setmetatable(lua, ObjectMetatable);
    return 1;
}

// 保护 userdata 元表，普通脚本无法手动调用析构或替换对象解释规则。
void InstallLua(lua_State* lua, const Registry& registry) {
    lua_pushlightuserdata(lua, const_cast<Registry*>(&registry)); lua_rawsetp(lua, LUA_REGISTRYINDEX, &registryKey);
    luaL_newmetatable(lua, ObjectMetatable);
    const luaL_Reg methods[]{{"__index", Safe<Index>}, {"__newindex", Safe<NewIndex>}, {"__len", Safe<Length>},
        {"__eq", Safe<Equal>}, {"__tostring", Safe<ToString>}, {"__gc", Safe<Destroy>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, methods, 0);
    lua_pushliteral(lua, "受保护的反射对象"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    const luaL_Reg functions[]{{"types", Safe<Types>}, {"type", Safe<Describe>}, {"functions", Safe<GlobalFunctions>},
        {"new", Safe<Construct>}, {"call", Safe<Call>}, {"invoke", Safe<InvokeExplicit>},
        {"get", Safe<GetField>}, {"set", Safe<SetField>}, {nullptr, nullptr}};
    lua_newtable(lua); luaL_setfuncs(lua, functions, 0); lua_setglobal(lua, "reflect");
}
}
