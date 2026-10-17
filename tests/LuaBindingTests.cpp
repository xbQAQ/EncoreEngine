#include "gameplay/ScriptSystem.h"
#include "gameplay/WorldReflection.h"
#include <iostream>
#include <map>

namespace {
using engine::reflection::TypeKind;
struct Base {
    int value{2}; // 验证派生对象对继承字段的访问。
    int Read() const { return value; }
};
class Counter : public Base {
public:
    static inline int limit{100}; // 不构造实例也应能访问的静态类配置。
    explicit Counter(int initial) { value = initial; }
    int Add(int amount) { return value += amount; }
    int Add(const std::string& text) { return value += static_cast<int>(text.size()); }
    static int Twice(int value) { return value * 2; }
    bool Alias(Counter& other) { value = 8; return other.value == 8; }
    void Throw() { throw std::runtime_error("测试 C++ 异常"); }
};
struct Bag {
    std::vector<Counter> counters{Counter{3}}; // 非默认可构造元素的容器。
    std::map<std::string, int> scores{{"first", 1}}; // 字符串键值容器。
    gameplay::Vec3 point{1, 2, 3}; // 用于验证只读路径和引用参数回写。
    void Append() { counters.emplace_back(8); }
    static void MovePoint(gameplay::Vec3& value) { value.x = 9; }
};
int HealthValue(const Base& value) { return value.value; }
void Move(gameplay::Vec3& value) { value.x += 10; }
void Increment(int& value) { ++value; }
bool Alias(gameplay::Vec3& left, gameplay::Vec3& right) { left.x = 8; return right.x == 8; }
void Overlap(Bag& bag, gameplay::Vec3& point) { bag.point.x = 8; point.y = 9; }
std::uint64_t Echo(std::uint64_t value) { return value; }
std::uint64_t TooLarge() { return std::numeric_limits<std::uint64_t>::max(); }

// 所有自定义类型都只登记元数据，Lua 侧没有对应的手工包装函数。
void Register(gameplay::ScriptSystem& scripts) {
    auto& types = scripts.Reflection();
    types.Type<Base>("Base").Constructor<>().Field<&Base::value>("value").Method<&Base::Read>("Read");
    types.Type<Counter>("Counter", TypeKind::Class).Base<Base>().Constructor<int>()
        .Method<static_cast<int(Counter::*)(int)>(&Counter::Add)>("Add")
        .Method<static_cast<int(Counter::*)(const std::string&)>(&Counter::Add)>("Add")
        .Method<&Counter::Twice>("Twice").Method<&Counter::Throw>("Throw").Method<&Counter::Alias>("Alias")
        .Field<&Counter::limit>("limit");
    types.Type<std::vector<Counter>>("Counters");
    types.Type<std::map<std::string, int>>("Scores");
    types.Type<Bag>("Bag").Constructor<>().Field<&Bag::counters>("counters").Field<&Bag::scores>("scores")
        .ReadOnly<&Bag::point>("readonlyPoint").Field<&Bag::point>("point").Method<&Bag::Append>("Append")
        .Method<&Bag::MovePoint>("MovePoint");
    types.Function<&HealthValue>("HealthValue");
    types.Function<&Move>("Move");
    types.Function<&Increment>("Increment");
    types.Function<&Alias>("Alias");
    types.Function<&Overlap>("Overlap");
    types.Function<&Echo>("Echo");
    types.Function<&TooLarge>("TooLarge");
}

void Run(const std::string& body, bool expectedSuccess) {
    gameplay::World world;
    const auto entity = world.CreateEntity("Binding test");
    world.Registry().emplace<gameplay::LuaScript>(entity).source = "function update(self, dt)\n" + body + "\nend";
    gameplay::ScriptSystem scripts;
    Register(scripts);
    const auto result = scripts.Compile(world, entity);
    if (!result.success) throw std::runtime_error(result.diagnostics);
    scripts.SetPlaying(true); scripts.Tick(world, 1);
    const auto diagnostics = std::string(scripts.Diagnostics(entity));
    if (diagnostics.empty() != expectedSuccess) throw std::runtime_error("Lua 通用绑定断言失败：" + diagnostics);
}

// 对象和容器均使用访问路径，保持父对象释放、容器增长后的有效访问。
void ValuesAndLifetime() {
    Run(R"(
local counter = reflect.new('Counter', 4)
assert(counter:Add(3) == 7)
assert(counter:Add('abc') == 10)
assert(counter:Read() == 10)
assert(reflect.call('HealthValue', counter) == 10)
assert(reflect.invoke('Counter', 'Twice', 3) == 6)
reflect.set('Counter', 'limit', 25)
assert(reflect.get('Counter', 'limit') == 25)
reflect.set(counter, 'value', 12)
assert(reflect.get(counter, 'value') == 12)
assert(reflect.type('Counter').bases[1] == 'Base')
local bag = reflect.new('Bag')
local first = bag.counters[1]
bag:Append()
assert(first.value == 3 and #bag.counters == 2)
first.value = 11
assert(bag.counters[1].value == 11)
assert(bag.counters[2]:Read() == 8)
bag.scores['second'] = 2
assert(#bag.scores == 2 and bag.scores['second'] == 2)
local child = bag.point
bag = nil
child.z = 9
assert(child.z == 9)
)", true);
}

// 非常量引用参数既能回写嵌套对象，也能以额外返回值表达标量输出。
void ReferenceParameters() {
    Run(R"(
local bag = reflect.new('Bag')
reflect.call('Move', bag.point)
assert(bag.point.x == 11)
assert(reflect.call('Increment', 4) == 5)
-- 同一路径的多个代理必须共享实参，成员函数的接收对象也参与别名匹配。
assert(reflect.call('Alias', bag.point, bag.point))
assert(bag.point.x == 8)
assert(bag.counters[1]:Alias(bag.counters[1]))
assert(bag.counters[1].value == 8)
-- 静态方法可以通过实例语法调用，但实例本身不参与接收对象的重叠检查。
bag:MovePoint(bag.point)
assert(bag.point.x == 9)
)", true);
}
}

int main() {
    try {
        ValuesAndLifetime();
        ReferenceParameters();
        // 只读路径、错误调用、溢出和 C++ 异常都应成为当前实例的诊断。
        for (const std::string body : {
             "local b=reflect.new('Bag'); b.readonlyPoint.x=3",
             "reflect.invoke('Counter', 'Add', 1)",
             "reflect.new('Counter', 1):Throw()",
             "reflect.call('Echo', -1)",
             "reflect.call('Echo', 18446744073709551616.0)",
             "reflect.call('TooLarge')",
             "reflect.get('Counter', 'value')",
             "local b=reflect.new('Bag'); reflect.call('Overlap', b, b.point)",
             "local b=reflect.new('Bag'); local p=b.counters[2]"}) Run(body, false);
        std::cout << "Lua generic binding tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
