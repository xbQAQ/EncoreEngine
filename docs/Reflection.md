# 运行时反射与 Lua 世界访问

反射系统覆盖类型、类、结构体、枚举、继承、构造函数、字段、属性、成员函数、静态函数和全局函数。C++20 通过显式、类型安全的注册提供元数据，Lua 不需要为每个新类型编写包装函数。未注册的 C++ 符号不会自动被发现。

`engine_reflection` 基于项目已有的 EnTT 3.15 meta，拥有独立上下文；`engine_lua_reflection` 将元数据和拥有型值开放给 Lua。Gameplay 的 `WorldReflection` 另行适配 ECS 组件，因此 Core 不依赖 EnTT，通用反射也不依赖 World 或 GPU。

## C++ 注册

使用 `ScriptSystem::Reflection()` 注册类型与函数，使用 `ReflectedWorld().RegisterComponent<T>()` 注册组件读写。必须在首次 `Compile()` 之前完成：首次编译会冻结注册表，`Clear()` 不解除冻结；每个 `ScriptSystem` 的注册表互相隔离。

```cpp
struct Health {
    float current{100}; // 当前生命值，不能低于零或超过上限。
    float maximum{100}; // 最大生命值，必须大于零。

    // 原生逻辑可以由动态调用复用，最终组件范围由事务校验保证。
    void Damage(float amount) { current -= amount; }
    float Ratio() const { return current / maximum; }
};

// 反射注册只描述能力，组件实例仍由宿主按实际业务创建。
auto& types = scripts.Reflection();
types.Type<Health>("Health", engine::reflection::TypeKind::Struct, "生命值组件")
    .Constructor<>()
    .Field<&Health::current>("current", "当前生命值")
    .Field<&Health::maximum>("maximum", "生命值上限")
    .Method<&Health::Damage>("Damage", "扣减生命值")
    .Method<&Health::Ratio>("Ratio", "剩余比例");

// 校验函数必须只读，抛出异常时整个回调的所有组件写入都会丢弃。
scripts.ReflectedWorld().RegisterComponent<Health>("Health",
    [](const Health& value, const Health&, const gameplay::World&) {
        if (!std::isfinite(value.current) || !std::isfinite(value.maximum) ||
            value.maximum <= 0 || value.current < 0 || value.current > value.maximum)
            throw std::invalid_argument("生命值范围无效");
    });
world.Registry().emplace<Health>(entity);
```

| 注册入口 | 用途 |
| --- | --- |
| `Type<T>(name, kind, description)` | 注册唯一的公开类型名和分类 |
| `.Constructor<Args...>()` | 显式开放构造签名 |
| `.Base<Parent>()` | 登记已经注册的基类，允许继承访问和向上转换 |
| `.Field<&T::member>(name)` | 注册字段，也支持静态字段指针 |
| `.ReadOnly<&T::member>(name)` | 只读成员字段 |
| `.Property<setter, getter>(name)` | 访问器属性，`nullptr` setter 表示只读 |
| `.Method<&T::method>(name)` | 成员或静态函数；重载使用明确的函数指针类型 |
| `.Value<Enum::Value>(name)` | 枚举命名常量 |
| `.Conversion<Target>()` | 登记原生类型转换 |
| `Function<&function>(name)` | 注册全局函数 |

字段和函数可以分别附带中文说明。类型名、字段名及哈希碰撞会被检查。构造函数只开放显式注册的签名，不能因为 C++ 类型可默认构造而意外开放宿主服务。

C++ 调用方可以使用 `Resolve()`、`Types()`、`Construct()`、`Invoke()`、`Call()`。`Registry::Info()` 返回分类及构造签名，EnTT 元数据提供字段、继承、参数和返回类型。调用方必须保证注册表比所有元数据和值存活更久。

## Lua 反射入口

| 调用 | 结果 |
| --- | --- |
| `reflect.types()` | 已注册的公开类型名数组 |
| `reflect.type('Health')` | 类型元数据表，也接受反射对象 |
| `reflect.functions()` | 全局函数签名数组 |
| `reflect.new('Health', ...)` | 动态构造拥有型对象 |
| `reflect.call('Dot', left, right)` | 动态调用全局函数 |
| `reflect.invoke(value, 'Damage', amount)` | 动态调用对象函数 |
| `reflect.invoke('Counter', 'Twice', value)` | 动态调用静态函数 |
| `reflect.get(value, 'field')` / `reflect.set(value, 'field', next)` | 对象字段读写 |
| `reflect.get('Counter', 'limit')` / `reflect.set('Counter', 'limit', next)` | 无需实例的静态字段读写 |

对象使用 `value.field` 读取、`value.field = next` 写入、`value:Method(...)` 调用。只读属性不能通过嵌套字段赋值绕过。反射序列使用一基整数下标，键值容器使用注册的键类型，`#value` 返回容器大小；序列下标必须已存在，键值容器赋值可以新增键。容器及元素类型也应注册。

类型元数据包含 `name`、`kind`、`description`、`size`、`fields`、`functions`、`bases`、`constructors` 和 `values`。`kind` 是 `primitive`、`enum`、`struct` 或 `class`。字段提供名称、说明、类型、`read_only` 和 `is_static`；函数提供名称、说明、返回类型、参数类型、`is_const` 和 `is_static`。构造函数是参数类型数组的数组，基类以公开名称列出。枚举常量保留强类型，例如 `reflect.type('LightType').values.Point`。

整数检查符号、范围与小数部分，浮点数拒绝 NaN、无穷和溢出，不接受数字字符串。返回 Lua 无法表达的超大 `uint64` 时报告错误。重载按数值匹配及对象类型选择，等优先级的候选会报告歧义。原始 C++ 指针不向脚本开放。

非常量对象引用参数会回写对象；标量引用无法原地修改 Lua 数字，作为额外返回值给出。相同根对象、相同访问路径的多个实参共享引用存储，接收对象也参与匹配。属性使用复制和 setter 回写，因此同一次调用中，互相包含的父子路径只要涉及可写引用就会被拒绝，应拆成独立调用。

## 世界组件

```lua
function update(self, dt)
    local health = world.get(self, 'Health')
    health:Damage(5 * dt)
    world.set(self, 'Health', health)

    for _, entity in ipairs(world.query('Light', 'Transform')) do
        local light = world.get(entity, 'Light')
        light.intensity = 20
        world.set(entity, 'Light', light)
    end
end
```

`world.entities()` 列出实体，`world.query(...)` 按组件交集筛选，`world.find(name)` 查找名称，`world.components()` 列出已适配组件，`world.components(entity)` 列出实体实际拥有的已适配组件，`world.has(entity, component)` 检查存在性。实体标识包含代际，过期标识、未知类型、缺失组件和错误快照类型会报错。

`world.get()` 返回独立快照，修改快照不自动写入 ECS。`world.set()` 校验并复制快照，同一回调后续读取能看到暂存结果。回调全部成功后，系统先完成所有校验，再无异常交换全部组件；出错时整批丢弃。旧 `position/rotation/scale` 便捷 API 使用同一事务。

组件必须可复制且支持 `noexcept swap`。自定义校验必须无副作用，不能修改世界或保留引用。提交只对无异常相等比较使用去重优化，否则显式写入会视为变化并推进 World 版本。

内置类型包括基础数值、字符串、`Vec3`、`Entity`、`LightType`、`Name`、`Transform`、`Velocity`、`Camera`、`Light`、`LuaScript`、`MeshHandle`、`MeshRenderer`、`ObjectIdentity` 和 `World`。数学全局函数包含 `Dot`、`Cross`、`Length`、`Normalize` 和 `IsFinite`。

`ObjectIdentity` 整个组件只读；网格资源身份、几何范围和光源发光资源槽只读；Lua 源码与路径由编辑器管理。`MeshRenderer.materialIndex` 只允许已有材质槽。World 类提供元数据，实际世界访问通过 `world` 服务，不开放直接构造或原始世界指针。

Lua 对象持有根值和访问路径，不长期保存 ECS、父对象或容器元素的地址。父代理释放后子代理仍拥有根值；容器变化后访问会重新解析并检查下标。注册原生函数仍须遵守 C++ 生命周期约束，不能自行保留实参地址。

## 验证

```powershell
cmake --build build --config Release --target reflection_tests lua_binding_tests lua_reflection_tests lua_script_tests
ctest --test-dir build -C Release --output-on-failure -R "^(reflection|lua_binding|lua_reflection|lua_scripts)$"
```

测试覆盖独立注册表、类与结构体、继承、构造和重载、静态及全局函数、只读访问器、非默认构造的容器元素、生命周期、引用参数别名、整数边界、C++ 异常以及真实 ECS 多组件事务。Lua 的执行预算及限制见 [`Scripting.md`](Scripting.md)。
