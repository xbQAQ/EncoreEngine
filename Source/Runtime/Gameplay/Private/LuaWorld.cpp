#include "ScriptContext.h"
#include "reflection/Lua.h"
extern "C" {
#include <lauxlib.h>
}
#include <algorithm>
#include <limits>

namespace gameplay::detail {
namespace {
using engine::reflection::GuardLua;
using engine::reflection::ReadLua;
using engine::reflection::PushLua;
using engine::reflection::Registry;
template<int(*Function)(lua_State*)> int Safe(lua_State* lua) { return GuardLua(lua, Function); }

// World 指针只在更新回调期间注入，源码初始化不能获得世界访问能力。
World& CurrentWorld(lua_State* lua) {
    auto* world = Context(lua).world;
    if (!world) throw std::invalid_argument("脚本初始化期间不能访问世界");
    return *world;
}
std::string Text(lua_State* lua, int index) {
    return ReadLua(lua, index, Context(lua).reflection->Types().Resolve("string")).cast<std::string>();
}
// 保留完整代际位并验证整数范围，禁止截断旧标识后访问复用的实体槽位。
Entity ReadEntity(lua_State* lua, int index = 1) {
    if (!lua_isinteger(lua, index)) throw std::invalid_argument("实体标识必须是整数");
    const auto id = lua_tointeger(lua, index);
    if (id < 0 || static_cast<lua_Unsigned>(id) > std::numeric_limits<entt::id_type>::max())
        throw std::invalid_argument("实体标识超出有效范围");
    const auto entity = static_cast<Entity>(static_cast<entt::id_type>(id));
    if (!CurrentWorld(lua).Registry().valid(entity)) throw std::invalid_argument("实体标识已失效");
    return entity;
}
// 读取与显式写入共享同一暂存表，Lua 对象始终拥有独立组件快照。
int Get(lua_State* lua) {
    auto& context = Context(lua);
    return PushLua(lua, context.reflection->Read(CurrentWorld(lua), ReadEntity(lua), Text(lua, 2), context.pending));
}
int Set(lua_State* lua) {
    auto& context = Context(lua);
    const auto entity = ReadEntity(lua);
    const auto name = Text(lua, 2);
    const auto& binding = context.reflection->Component(name);
    auto value = ReadLua(lua, 3, binding.type);
    context.reflection->Stage(CurrentWorld(lua), entity, name, value, context.pending);
    return 0;
}
int Has(lua_State* lua) {
    const auto entity = ReadEntity(lua);
    lua_pushboolean(lua, Context(lua).reflection->Component(Text(lua, 2)).has(CurrentWorld(lua), entity));
    return 1;
}
// 使用当前实体存储而非组件池遍历，并排序以固定查询和同名查找的结果。
std::vector<Entity> Entities(lua_State* lua) {
    std::vector<Entity> entities;
    for (const auto [entity] : CurrentWorld(lua).Registry().storage<Entity>().each()) entities.push_back(entity);
    std::sort(entities.begin(), entities.end(), [](auto a, auto b) { return entt::to_integral(a) < entt::to_integral(b); });
    return entities;
}
// 组件适配提供存在性检查，新增自定义组件不需要增加专用查询函数。
int Query(lua_State* lua) {
    auto& context = Context(lua);
    std::vector<const ComponentBinding*> required;
    for (int index = 1; index <= lua_gettop(lua); ++index) required.push_back(&context.reflection->Component(Text(lua, index)));
    lua_newtable(lua); int index = 0;
    for (const auto entity : Entities(lua)) {
        if (!std::all_of(required.begin(), required.end(), [&](const auto* binding) { return binding->has(CurrentWorld(lua), entity); })) continue;
        lua_pushinteger(lua, entt::to_integral(entity)); lua_rawseti(lua, -2, ++index);
    }
    return 1;
}
// 名称也从暂存表读取，使同一回调内的重命名立即影响后续查找。
int Find(lua_State* lua) {
    auto& context = Context(lua);
    const auto expected = Text(lua, 1);
    for (const auto entity : Entities(lua)) {
        if (!CurrentWorld(lua).Registry().all_of<Name>(entity)) continue;
        const auto name = context.reflection->Read(CurrentWorld(lua), entity, "Name", context.pending);
        if (name.cast<const Name&>().value == expected) { lua_pushinteger(lua, entt::to_integral(entity)); return 1; }
    }
    lua_pushnil(lua); return 1;
}
int Components(lua_State* lua) {
    // 无参数枚举适配能力，有实体参数时只返回该实体实际拥有的组件。
    const bool filter = lua_gettop(lua) != 0;
    const auto entity = filter ? ReadEntity(lua) : entt::null;
    lua_newtable(lua); int index = 0;
    for (const auto& [id, binding] : Context(lua).reflection->Components()) {
        if (filter && !binding.has(CurrentWorld(lua), entity)) continue;
        const auto name = Context(lua).reflection->Types().Name(binding.type);
        lua_pushlstring(lua, name.data(), name.size()); lua_rawseti(lua, -2, ++index);
    }
    return 1;
}
// 有效性探测返回布尔值，其他实际访问接口仍对无效实体报告错误。
int Valid(lua_State* lua) {
    const auto id = lua_isinteger(lua, 1) ? lua_tointeger(lua, 1) : -1;
    auto* world = Context(lua).world;
    lua_pushboolean(lua, world && id >= 0 && static_cast<lua_Unsigned>(id) <= std::numeric_limits<entt::id_type>::max() &&
        world->Registry().valid(static_cast<Entity>(static_cast<entt::id_type>(id))));
    return 1;
}
// 位姿便捷入口消费与通用反射相同的快照，保持已有脚本的事务行为。
int ReadTransform(lua_State* lua) {
    auto& context = Context(lua);
    const auto entity = ReadEntity(lua);
    auto value = context.reflection->Read(CurrentWorld(lua), entity, "Transform", context.pending);
    const auto& transform = value.cast<const Transform&>();
    const auto kind = lua_tointeger(lua, lua_upvalueindex(1));
    const auto& vector = kind == 0 ? transform.position : kind == 1 ? transform.rotationDegrees : transform.scale;
    lua_pushnumber(lua, vector.x); lua_pushnumber(lua, vector.y); lua_pushnumber(lua, vector.z);
    return 3;
}
int WriteTransform(lua_State* lua) {
    auto& context = Context(lua);
    const auto entity = ReadEntity(lua);
    auto value = context.reflection->Read(CurrentWorld(lua), entity, "Transform", context.pending);
    const auto scalar = context.reflection->Types().Resolve("float");
    const Vec3 vector{ReadLua(lua, 2, scalar).cast<float>(), ReadLua(lua, 3, scalar).cast<float>(), ReadLua(lua, 4, scalar).cast<float>()};
    auto& transform = value.cast<Transform&>();
    const auto kind = lua_tointeger(lua, lua_upvalueindex(1));
    (kind == 0 ? transform.position : kind == 1 ? transform.rotationDegrees : transform.scale) = vector;
    context.reflection->Stage(CurrentWorld(lua), entity, "Transform", value, context.pending);
    return 0;
}
}

void RegisterWorldApi(lua_State* lua) {
    const luaL_Reg functions[]{{"get", Safe<Get>}, {"set", Safe<Set>}, {"has", Safe<Has>}, {"find", Safe<Find>},
        {"query", Safe<Query>}, {"entities", Safe<Query>}, {"components", Safe<Components>}, {"valid", Safe<Valid>}, {nullptr, nullptr}};
    lua_newtable(lua); luaL_setfuncs(lua, functions, 0);
    // 旧接口和通用组件接口共享事务，迁移现有脚本时不会出现两套位姿状态。
    const char* readers[]{"position", "rotation", "scale"};
    const char* writers[]{"set_position", "set_rotation", "set_scale"};
    for (int index = 0; index < 3; ++index) {
        lua_pushinteger(lua, index); lua_pushcclosure(lua, Safe<ReadTransform>, 1); lua_setfield(lua, -2, readers[index]);
        lua_pushinteger(lua, index); lua_pushcclosure(lua, Safe<WriteTransform>, 1); lua_setfield(lua, -2, writers[index]);
    }
    lua_setglobal(lua, "world");
}
}
