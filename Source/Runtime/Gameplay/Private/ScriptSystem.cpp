#include "gameplay/ScriptSystem.h"
#include "gameplay/World.h"
#include "ScriptContext.h"
#include "reflection/Lua.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <unordered_map>

namespace gameplay {
namespace {
constexpr std::size_t MemoryLimit = 8 * 1024 * 1024;
using detail::Instance;
using detail::Context;

void* Allocate(void* user, void* pointer, std::size_t oldSize, std::size_t newSize) {
    auto& instance = *static_cast<Instance*>(user);
    // 首次分配时 Lua 传入的是类型标记，而不是旧内存块大小。
    if (!pointer) oldSize = 0;
    if (!newSize) { instance.allocated -= oldSize; std::free(pointer); return nullptr; }
    if (newSize > MemoryLimit || instance.allocated - oldSize > MemoryLimit - newSize) return nullptr;
    void* replacement = std::realloc(pointer, newSize);
    if (replacement) instance.allocated = instance.allocated - oldSize + newSize;
    return replacement;
}
void InstructionLimit(lua_State* lua, lua_Debug*) {
    if (--Context(lua).instructions <= 0) luaL_error(lua, "instruction budget exceeded");
}
void RegisterApi(lua_State* lua) {
    // 游戏逻辑虚拟机不开放 io、os、package、debug 及文件加载器。
    for (const auto& [name, open] : {std::pair{LUA_GNAME, luaopen_base}, {LUA_MATHLIBNAME, luaopen_math},
                                  {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table}}) {
        luaL_requiref(lua, name, open, 1); lua_pop(lua, 1);
    }
    // 脚本自行捕获异常可能绕过指令预算，因此错误恢复只由宿主执行。
    for (const char* name : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall"}) {
        lua_pushnil(lua); lua_setglobal(lua, name);
    }
    // 通用反射不读取世界，只有 World 适配层在更新回调期间借用 ECS。
    engine::reflection::InstallLua(lua, Context(lua).reflection->Types());
    detail::RegisterWorldApi(lua);
}
std::string LuaError(lua_State* lua, const char* prefix) {
    const char* detail = lua_tostring(lua, -1);
    std::string message = std::string(prefix) + (detail ? detail : "unknown Lua error");
    lua_pop(lua, 1);
    return message;
}
}
struct ScriptSystem::State {
    WorldReflection reflection; // 最后销毁注册表，保证所有虚拟机值先释放。
    std::unordered_map<Entity, std::unique_ptr<Instance>> instances; // 以带代际的实体标识索引，调用之间不保留 ECS 指针。
};
ScriptSystem::ScriptSystem() : state_(std::make_unique<State>()) {}
ScriptSystem::~ScriptSystem() = default;
ScriptResult ScriptSystem::Compile(World& world, Entity entity) {
    if (!world.Registry().valid(entity) || !world.Registry().all_of<LuaScript>(entity))
        return {false, "Lua 编译失败：实体或脚本组件无效"};
    const auto& authored = world.Registry().get<LuaScript>(entity);
    auto& current = state_->instances[entity];
    if (!current) current = std::make_unique<Instance>();
    current->attempted = authored.source;
    auto candidate = std::make_unique<Instance>();
    // 首次编译后固定签名；自定义类型和组件应在此之前完成注册。
    state_->reflection.Types().Freeze();
    candidate->reflection = &state_->reflection;
    candidate->source = candidate->attempted = authored.source;
    candidate->lua = lua_newstate(Allocate, candidate.get());
    if (!candidate->lua) return {false, current->diagnostics = "Lua 编译失败：无法创建虚拟机"};
    auto* lua = candidate->lua;
    *static_cast<Instance**>(lua_getextraspace(lua)) = candidate.get();
    RegisterApi(lua);
    candidate->instructions = 100;
    lua_sethook(lua, InstructionLimit, LUA_MASKCOUNT, 1000);
    // 源码初始化受异常保护且不能修改 World，编译失败不会留下部分场景改动。
    std::string_view chunk = authored.source;
    // Windows 编辑器可能写入 UTF-8 BOM；缓冲区加载器会将其误认作源码标记。
    if (chunk.starts_with("\xef\xbb\xbf")) chunk.remove_prefix(3);
    if (luaL_loadbufferx(lua, chunk.data(), chunk.size(), authored.path.empty() ? "Gameplay.lua" : authored.path.c_str(), "t") != LUA_OK ||
        lua_pcall(lua, 0, 0, 0) != LUA_OK)
        return {false, current->diagnostics = LuaError(lua, "Lua 编译失败：")};
    lua_getglobal(lua, "update");
    const bool valid = lua_isfunction(lua, -1); lua_pop(lua, 1);
    if (!valid) return {false, current->diagnostics = "Lua 编译失败：必须定义 update(self, dt) 函数"};
    current = std::move(candidate);
    return {true, {}};
}
void ScriptSystem::Tick(World& world, float deltaSeconds) {
    // 读取当前注册表中的源码前，先清理已失效的实体代际。
    std::erase_if(state_->instances, [&](const auto& pair) {
        return !world.Registry().valid(pair.first) || !world.Registry().all_of<LuaScript>(pair.first);
    });
    if (!playing_ || !std::isfinite(deltaSeconds) || deltaSeconds <= 0) return;
    for (const auto [entity, authored] : world.Registry().view<LuaScript>().each()) {
        if (!authored.enabled) continue;
        const auto found = state_->instances.find(entity);
        if (found == state_->instances.end() || found->second->attempted != authored.source) Compile(world, entity);
        auto& instance = *state_->instances.at(entity);
        if (!instance.lua || instance.faulted) continue;
        instance.world = &world; instance.pending.clear(); instance.instructions = 100;
        lua_getglobal(instance.lua, "update");
        lua_pushinteger(instance.lua, static_cast<lua_Integer>(entt::to_integral(entity)));
        lua_pushnumber(instance.lua, deltaSeconds);
        const bool success = lua_pcall(instance.lua, 2, 0, 0) == LUA_OK;
        instance.world = nullptr;
        if (!success) {
            instance.diagnostics = LuaError(instance.lua, "Lua 执行失败：");
            instance.faulted = true; instance.pending.clear(); continue;
        }
        // 先校验整个组件事务，再以无异常交换发布，错误不能留下部分世界修改。
        try { state_->reflection.Commit(world, instance.pending); }
        catch (const std::exception& error) {
            instance.diagnostics = std::string("Lua 组件提交失败：") + error.what();
            instance.faulted = true; instance.pending.clear();
        }
    }
}
std::string_view ScriptSystem::Diagnostics(Entity entity) const {
    const auto found = state_->instances.find(entity);
    return found == state_->instances.end() ? std::string_view{} : found->second->diagnostics;
}
void ScriptSystem::Clear() { state_->instances.clear(); playing_ = false; }
engine::reflection::Registry& ScriptSystem::Reflection() { return state_->reflection.Types(); }
WorldReflection& ScriptSystem::ReflectedWorld() { return state_->reflection; }
std::string_view ScriptSystem::DefaultSource() {
    return "-- 每个模拟帧调用一次，self 是带代际的实体标识。\nfunction update(self, dt)\n    local x, y, z = world.rotation(self)\n    world.set_rotation(self, x, y + 30 * dt, z)\nend\n";
}
}
