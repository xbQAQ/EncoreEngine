#include "gameplay/ScriptSystem.h"
#include "gameplay/World.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        gameplay::World world;
        const auto mover = world.CreateEntity("Lua mover");
        auto& source = world.Registry().emplace<gameplay::LuaScript>(mover);
        source.source = "\xef\xbb\xbf" "function update(self, dt) local x,y,z=world.position(self); world.set_position(self,x+dt,y,z) end";
        gameplay::ScriptSystem scripts;
        Check(scripts.Compile(world, mover).success, "valid Lua compiles");
        scripts.Tick(world, 0.25f);
        Check(world.Registry().get<gameplay::Transform>(mover).position.x == 0, "paused scripts do not run");
        scripts.SetPlaying(true);
        scripts.Tick(world, 0.25f);
        Check(std::abs(world.Registry().get<gameplay::Transform>(mover).position.x - 0.25f) < 1e-5f, "Lua updates actual ECS pose");
        source.source = "function update(";
        Check(!scripts.Compile(world, mover).success, "invalid Lua reports diagnostics");
        scripts.Tick(world, 0.25f);
        Check(std::abs(world.Registry().get<gameplay::Transform>(mover).position.x - 0.5f) < 1e-5f, "failed compile retains last good script");
        source.source = "function update(self,dt) world.set_position(self,99,0,0); error('broken') end";
        Check(scripts.Compile(world, mover).success, "runtime failure source compiles");
        scripts.Tick(world, 0.25f);
        Check(world.Registry().get<gameplay::Transform>(mover).position.x == 0.5f, "failed update commits no partial ECS writes");
        Check(!scripts.Diagnostics(mover).empty(), "isolated runtime error is available to editor");
        const auto survivor = world.CreateEntity("Survivor");
        world.Registry().emplace<gameplay::LuaScript>(survivor).source =
            "function update(self,dt) world.set_position(self,3,0,0) end";
        scripts.Tick(world, 0.25f);
        Check(world.Registry().get<gameplay::Transform>(survivor).position.x == 3, "other scripts survive one script failure");
        world.Registry().get<gameplay::LuaScript>(mover).source = "function update(self,dt) while true do end end";
        Check(scripts.Compile(world, mover).success, "loop source compiles");
        scripts.Tick(world, 0.25f);
        Check(!scripts.Diagnostics(mover).empty(), "instruction budget terminates infinite update");
        // 过期代际不能修改复用同一 ECS 槽位的新实体。
        const auto stale = world.CreateEntity("Destroyed target");
        const auto staleId = entt::to_integral(stale);
        world.DestroyEntity(stale);
        const auto replacement = world.CreateEntity("Replacement target");
        world.Registry().get<gameplay::LuaScript>(mover).source =
            "function update(self,dt) world.set_position(" + std::to_string(staleId) + ",88,0,0) end";
        Check(scripts.Compile(world, mover).success, "stale-ID source compiles");
        scripts.Tick(world, 0.25f);
        Check(world.Registry().get<gameplay::Transform>(replacement).position.x == 0 && !scripts.Diagnostics(mover).empty(),
            "stale entity IDs are rejected before accessing replacement components");
        Check(world.DestroyEntity(mover), "destroy scripted entity");
        scripts.Tick(world, 0.25f);
        std::cout << "Lua gameplay tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
