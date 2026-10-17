#pragma once
#include "gameplay/Components.h"
#include <memory>
#include <string>
#include <string_view>

namespace engine::reflection { class Registry; }
namespace gameplay {
class World;
class WorldReflection;
struct ScriptResult {
    bool success{}; // 候选脚本是否已成功编译并初始化。
    std::string diagnostics; // UTF-8 中文提示及 Lua 原始编译或执行诊断。
};

// 每个实体独占隔离的 Lua 虚拟机，ECS 边界只接受校验过的带代际标识。
class ScriptSystem {
public:
    ScriptSystem();
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;
    ScriptResult Compile(World&, Entity);
    void Tick(World&, float deltaSeconds);
    void SetPlaying(bool playing) { playing_ = playing; }
    bool Playing() const { return playing_; }
    std::string_view Diagnostics(Entity) const;
    void Clear();
    // 引擎扩展在首次编译之前注册；Lua 自动消费相同的类型与组件描述。
    engine::reflection::Registry& Reflection();
    WorldReflection& ReflectedWorld();
    static std::string_view DefaultSource();
private:
    struct State;
    std::unique_ptr<State> state_; // 独占虚拟机状态，Lua 原生类型仅在模块内部可见。
    bool playing_{}; // 暂停保留虚拟机状态，但不执行游戏逻辑回调。
};
}
