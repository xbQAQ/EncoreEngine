#include "reflection/Registry.h"
#include <array>
#include <iostream>

namespace {
using engine::reflection::Registry;
using engine::reflection::TypeKind;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void Reject(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "无效反射操作未被拒绝");
}

struct Base {
    int value{1}; // 用于验证继承字段与基类转换的状态。
    int Read() const { return value; }
};
class Counter : public Base {
public:
    explicit Counter(int initial) { value = initial; }
    int Add(int amount) { return value += amount; }
    int Add(const std::string& text) { return value += static_cast<int>(text.size()); }
    void Set(int next) { value = next; }
    static int Twice(int value) { return value * 2; }
};
enum class Mode {
    First, // 第一种测试状态。
    Second, // 第二种测试状态。
};
int Sum(int left, int right) { return left + right; }

// 独立自定义类验证完整注册接口，不依赖引擎组件的专用绑定。
void DynamicReflection() {
    Registry registry;
    registry.Type<Base>("Base", TypeKind::Struct).Constructor<>().Field<&Base::value>("value").Method<&Base::Read>("read");
    registry.Type<Counter>("Counter", TypeKind::Class).Base<Base>().Constructor<int>()
        .Method<static_cast<int(Counter::*)(int)>(&Counter::Add)>("add")
        .Method<static_cast<int(Counter::*)(const std::string&)>(&Counter::Add)>("add")
        .Method<&Counter::Twice>("twice")
        .Property<&Counter::Set, &Base::Read>("current")
        .Property<nullptr, &Base::Read>("readonly");
    registry.Type<Mode>("Mode", TypeKind::Enumeration).Value<Mode::First>("First").Value<Mode::Second>("Second");
    registry.Function<&Sum>("sum");
    std::array arguments{entt::meta_any{registry.Context(), 5}};
    auto object = registry.Construct("Counter", arguments);
    Check(object.cast<Counter&>().value == 5, "动态构造失败");
    Check(registry.Invoke(object, "add", arguments).cast<int>() == 10, "成员函数调用失败");
    std::array text{entt::meta_any{registry.Context(), std::string("abc")}};
    Check(registry.Invoke(object, "add", text).cast<int>() == 13, "函数重载调用失败");
    Check(registry.Invoke(object, "read").cast<int>() == 13, "继承函数调用失败");
    Check(object.type().data(Registry::Id("value")).set(object, 20), "继承字段写入失败");
    Check(!object.type().data(Registry::Id("readonly")).set(object, 1), "只读属性可被修改");
    Check(object.type().data(Registry::Id("current")).set(object, 21), "属性访问器写入失败");
    Check(object.cast<Counter&>().value == 21, "属性访问器没有执行");
    auto result = registry.Resolve("Counter").invoke(Registry::Id("twice"), {}, arguments.data(), arguments.size());
    Check(result.cast<int>() == 10, "静态函数调用失败");
    std::array sum{entt::meta_any{registry.Context(), 2}, entt::meta_any{registry.Context(), 3}};
    Check(registry.Call("sum", sum).cast<int>() == 5, "全局函数调用失败");
    Check(registry.Resolve("Mode").data(Registry::Id("Second")).get({}).cast<Mode>() == Mode::Second, "枚举常量反射失败");
    Registry independent;
    Check(!independent.Resolve("Counter"), "反射上下文发生串扰");
    Reject([&] { independent.Invoke(object, "read"); });
    Reject([&] { registry.Type<Counter>("Counter"); });
    registry.Freeze();
    Reject([&] { registry.Function<&Sum>("another"); });
}
}

int main() {
    try {
        DynamicReflection();
        std::cout << "Runtime reflection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
