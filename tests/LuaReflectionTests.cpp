#include "gameplay/ScriptSystem.h"
#include "gameplay/World.h"
#include "gameplay/WorldReflection.h"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// 每个场景使用真实虚拟机和独立世界，避免前一项的错误状态污染后续断言。
void Run(const std::string& source, bool expectedSuccess, const auto& verify) {
    gameplay::World world;
    const auto entity = world.CreateEntity("Actor");
    const auto lamp = world.CreateEntity("Lamp");
    world.Registry().emplace<gameplay::Light>(lamp);
    world.Registry().emplace<gameplay::Camera>(entity);
    world.Registry().emplace<gameplay::LuaScript>(entity).source = source;
    gameplay::ScriptSystem scripts;
    const auto compilation = scripts.Compile(world, entity);
    Check(compilation.success, "反射脚本初始化失败：" + compilation.diagnostics);
    scripts.SetPlaying(true);
    scripts.Tick(world, 0.25f);
    Check(scripts.Diagnostics(entity).empty() == expectedSuccess,
          "反射脚本执行结果不符合预期：" + std::string(scripts.Diagnostics(entity)));
    verify(world, entity, lamp);
}

// 类型发现、构造和嵌套字段修改必须经过统一反射入口。
void MetadataAndValues() {
    Run(R"(
assert(reflect.type('Transform').kind == 'struct')
assert(reflect.type('World').kind == 'class')
assert(reflect.type('float').kind == 'primitive')
assert(#reflect.types() >= 15)
local p = reflect.new('Vec3', 1, 2, 3)
assert(p.x == 1 and p.y == 2 and p.z == 3)
assert(reflect.call('Dot', p, p) == 14)
local t = reflect.new('Transform')
local nested = t.position
t = nil
collectgarbage = nil
nested.x = 7
assert(nested.x == 7)
function update(self, dt)
    local pose = world.get(self, 'Transform')
    pose.position.x = p.x + 4
    pose.rotationDegrees.y = 30
    world.set(self, 'Transform', pose)
    local x = world.position(self)
    assert(x == 5)
end
)", true, [](const auto& world, auto entity, auto) {
        const auto& pose = world.Registry().template get<gameplay::Transform>(entity);
        Check(pose.position.x == 5 && pose.rotationDegrees.y == 30, "反射修改没有提交到真实 ECS");
    });
}

// 查询和多组件写入共享一个回调事务，旧 API 与新 API 必须观察到同一暂存状态。
void QueryAndComponents() {
    Run(R"(
function update(self, dt)
    assert(#world.entities() == 2)
    assert(#world.components() == 8)
    local names = {}
    for _, name in ipairs(world.components(self)) do names[name] = true end
    assert(names.Transform and names.Camera and names.LuaScript and not names.Light)
    assert(#world.query('Light', 'Transform') == 1)
    local lamp = world.find('Lamp')
    assert(lamp and world.has(lamp, 'Light'))
    assert(not world.has(self, 'Light'))
    local light = world.get(lamp, 'Light')
    light.type = reflect.type('LightType').values.Point
    light.intensity = 42
    light.color.x = 0.25
    world.set(lamp, 'Light', light)
    assert(world.get(lamp, 'Light').intensity == 42)
    world.set_position(self, 3, 0, 0)
    local pose = world.get(self, 'Transform')
    assert(pose.position.x == 3)
    pose.position.y = 4
    world.set(self, 'Transform', pose)
end
)", true, [](const auto& world, auto entity, auto lamp) {
        const auto& light = world.Registry().template get<gameplay::Light>(lamp);
        Check(light.intensity == 42 && light.color.x == 0.25f && light.type == gameplay::LightType::Point,
              "光源组件没有通过反射更新");
        const auto& pose = world.Registry().template get<gameplay::Transform>(entity);
        Check(pose.position.x == 3 && pose.position.y == 4, "新旧位姿接口的事务不一致");
    });
}

// 任一组件校验失败时，之前排队的名称、位姿和光源编辑都不能泄漏到世界。
void Rollback() {
    Run(R"(
function update(self, dt)
    local name = world.get(self, 'Name')
    name.value = 'Changed'
    world.set(self, 'Name', name)
    world.set_position(self, 99, 0, 0)
    local lamp = world.find('Lamp')
    local light = world.get(lamp, 'Light')
    light.intensity = -1
    world.set(lamp, 'Light', light)
end
)", false, [](const auto& world, auto entity, auto lamp) {
        Check(world.Registry().template get<gameplay::Name>(entity).value == "Actor", "失败回调泄漏名称修改");
        Check(world.Registry().template get<gameplay::Transform>(entity).position.x == 0, "失败回调泄漏位姿修改");
        Check(world.Registry().template get<gameplay::Light>(lamp).intensity == 15, "失败回调泄漏光源修改");
    });
}

// 错误类型、整数越界、缺失成员和只读属性必须转为脚本诊断，不能破坏宿主。
void InvalidAccess() {
    for (const std::string operation : {
             "reflect.new('Vec3', 'bad', 0, 0)",
             "local p=reflect.new('Vec3'); p.missing=1",
             "local p=reflect.new('Vec3'); p.x=0/0",
             "world.get(self, 'Missing')",
             "world.get(4294967296, 'Transform')",
             "local s=world.get(self, 'LuaScript'); s.source='bad'",
             "world.set(self, 'Camera', world.get(self, 'Transform'))"}) {
        Run("function update(self, dt) " + operation + " end", false, [](const auto&, auto, auto) {});
    }
}

struct ThrowingEquality {
    int value{}; // 比较可能抛异常的自定义组件状态。
    bool operator==(const ThrowingEquality& other) const {
        if (value != other.value) throw std::runtime_error("组件比较异常");
        return true;
    }
};

// 比较优化不能让组件提交产生异常，否则前面的位姿交换会泄漏到世界。
void NoThrowCommit() {
    gameplay::World world;
    const auto entity = world.CreateEntity("Atomic test");
    world.Registry().emplace<ThrowingEquality>(entity);
    world.Registry().emplace<gameplay::LuaScript>(entity).source = R"(
function update(self, dt)
    world.set_position(self, 7, 0, 0)
    local value = world.get(self, 'ThrowingEquality')
    value.value = 9
    world.set(self, 'ThrowingEquality', value)
end
)";
    gameplay::ScriptSystem scripts;
    scripts.Reflection().Type<ThrowingEquality>("ThrowingEquality").Field<&ThrowingEquality::value>("value");
    scripts.ReflectedWorld().RegisterComponent<ThrowingEquality>("ThrowingEquality");
    Check(scripts.Compile(world, entity).success, "自定义组件脚本编译失败");
    const auto revision = world.Revision();
    scripts.SetPlaying(true); scripts.Tick(world, 1);
    Check(scripts.Diagnostics(entity).empty(), "可能抛异常的比较不应进入提交阶段");
    Check(world.Registry().get<gameplay::Transform>(entity).position.x == 7 &&
          world.Registry().get<ThrowingEquality>(entity).value == 9 && world.Revision() > revision,
          "自定义组件事务没有完整提交并推进世界版本");
}
}

int main() {
    try {
        MetadataAndValues();
        QueryAndComponents();
        Rollback();
        InvalidAccess();
        NoThrowCommit();
        std::cout << "Lua reflection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
