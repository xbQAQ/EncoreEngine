#pragma once
#include "gameplay/WorldReflection.h"
extern "C" {
#include <lua.h>
}

namespace gameplay::detail {
struct Instance {
    lua_State* lua{}; // 在上下文和组件适配销毁前关闭虚拟机。
    std::size_t allocated{}; // 当前虚拟机的 Lua 分配字节数。
    std::string source; // 最近成功初始化的源代码。
    std::string attempted; // 最近尝试编译的草稿，避免逐帧重试相同错误。
    std::string diagnostics; // 最近一次编译或运行错误。
    World* world{}; // 只在更新回调期间借用世界，初始化时不可访问。
    WorldReflection* reflection{}; // 所属 ScriptSystem 的类型与组件注册表。
    ComponentWrites pending; // 同一个回调内所有组件的拥有型写入快照。
    int instructions{}; // 剩余的指令钩子触发次数。
    bool faulted{}; // 运行错误只暂停当前实例。
    ~Instance() { if (lua) lua_close(lua); }
};
inline Instance& Context(lua_State* lua) { return **static_cast<Instance**>(lua_getextraspace(lua)); }
void RegisterWorldApi(lua_State* lua);
}
