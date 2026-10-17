#pragma once
#include "reflection/Registry.h"

struct lua_State;

namespace engine::reflection {
// 安装统一反射入口；调用方保证注册表比虚拟机存活更久。
void InstallLua(lua_State* lua, const Registry& registry);
entt::meta_any ReadLua(lua_State* lua, int index, entt::meta_type expected);
int PushLua(lua_State* lua, entt::meta_any value);
// 在 C++ 临时对象析构之后抛出 Lua 错误，避免普通类型错误跨越析构边界。
int GuardLua(lua_State* lua, int (*function)(lua_State*));
}
