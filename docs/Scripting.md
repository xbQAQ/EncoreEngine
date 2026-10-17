# 脚本系统

本版本提供两条独立的执行链：HLSL 表面脚本参与实际材质求值；Lua 5.4.8 游戏逻辑脚本按模拟帧更新 ECS 实体。源码编辑、源文件路径、组件启用状态和对象材质分配属于编辑数据，可参与撤销和重做。编译产物、Lua 虚拟机内部状态和逐帧模拟位姿不作为源码编辑历史保存。

## 编辑与源文件

选择对象后，属性栏提供“材质脚本 / HLSL”和“游戏逻辑 / Lua”区域。可创建源码、直接编辑、打开文件、保存或另存为。文件路径使用 UTF-8 保存，原生文件对话框支持中文目录；打开的单个源文件上限为 1 MiB。HLSL 和 Lua 缓冲区编译均接受 UTF-8 BOM。

源码位于 CPU 编辑数据中：HLSL 保存在 `MaterialResource::ScriptSource()` 与 `ScriptPath()`，Lua 保存在实体的 `LuaScript` 组件中。源文件保存只写源码，不替代场景保存。修改外部文件后需要重新打开文件，本版本没有外部文件自动监视器。

## HLSL 表面脚本

每份材质脚本必须定义下面的函数：

```hlsl
// 标准纹理因子求值后执行，所有颜色都使用线性空间。
void Surface(inout Material material, float2 uv, float4 tangent)
{
    material.baseRoughness.xyz = float3(0.1, 0.7, 0.2);
    material.baseRoughness.w = 0.35;
}
```

`uv` 是当前表面导入的纹理坐标，`tangent.xyz` 是表面切线，`tangent.w` 是 UV 手性。缺少 UV 的几何使用现有渲染器的回退值。函数在标准纹理采样之后执行，可读取或修改下列光学参数：

| 字段 | 含义 |
| --- | --- |
| `baseRoughness` | 线性基础色 RGB、感知粗糙度 |
| `emissionMetallic` | 线性自发光 RGB、金属度 |
| `transmissionIor` | 透射比例、IOR、清漆比例、清漆光泽 |
| `principled` | 高光强度、高光染色、各向异性、次表面或平整度混合 |
| `sheen` | 绒毛量、绒毛染色、薄表面平整度、漫透射 |
| `medium` | 线性吸收颜色与衰减距离 |
| `extras.x` | 各向异性旋转角，单位为弧度 |
| `tangentFrame` | 光学各向异性使用的可选切线覆盖 |

脚本输出的基础色和自发光限制为非负，粗糙度限制在 `[0.001, 1]`，金属度限制在 `[0, 1]`。脚本应输出有限数值，并遵循相应材质参数的物理范围。

`flags`、`textures`、`textures2` 和 `extras.yzw` 在脚本执行后恢复为作者配置，因此脚本不会更换纹理绑定、修改 Alpha 覆盖或改变法线贴图强度。几何双面性、透明裁剪和法线贴图仍由材质属性控制。透射因子变化也不替代作者配置的几何接受规则。

这是现有 Disney 材质模型的表面参数扩展点，不提供自定义 BSDF、顶点位移、自定义渲染入口、描述符绑定或任意 GPU 资源创建。渲染器和编辑器均保留原有模块职责。

### 编译与实时预览

“编译并预览”显式发起编译；启用“实时预览”时，连续编辑稳定约 0.45 秒后自动发起。源码撤销或重做同样会触发预览更新。编译使用构建时配置的 Windows SDK DXC 和项目的原始着色器目录，需要这些开发文件仍然可用。

工作线程持有源码副本，只生成 DXIL；主线程继续渲染上一版可用材质。整组候选程序覆盖软件查询、可用时的 DXR，以及独立纯光栅化排列。纯光栅化集合不包含路径追踪或 ReSTIR 排列。新编辑到达时只保留最新待编译草稿，已过期结果不会短暂发布。

所有源码和管线都创建成功后，渲染线程等待已有 GPU 工作结束并整体替换 PSO，再重置累积历史。任何 DXC 或原生 PSO 创建失败都保留上一整组可用管线，错误保留在面板中。DXC 诊断中的 `Material_<槽位>.hlsl` 行号对应该材质脚本的源码行号。

## Lua 游戏逻辑

实体可以附加一份 `LuaScript`。每个实体独占一个 Lua 虚拟机，源码必须定义 `update(self, dt)`：

```lua
-- self 是带代际的实体标识，dt 是当前模拟帧的秒数。
function update(self, dt)
    local x, y, z = world.rotation(self)
    world.set_rotation(self, x, y + 30 * dt, z)
end
```

“运行”和“暂停”共用场景模拟开关；“启用脚本”控制单个实体。显式“编译 Lua”可以在暂停时使用。开始运行后，系统也会编译新增或已改动的启用脚本。暂停保留虚拟机内部状态；成功重新编译会创建新虚拟机，因此脚本局部状态重新初始化。

### 实际可用 API

下列函数位于全局 `world` 表中。实体标识是整数，包含 EnTT 的代际信息，不能用一个实体的旧标识访问后来复用槽位的新实体。

| 调用 | 返回值或行为 |
| --- | --- |
| `world.valid(entity)` | 返回实体标识当前是否有效 |
| `world.entities()` | 返回按实体标识排序的全部有效实体 |
| `world.query('Light', 'Transform')` | 返回同时具有指定组件的实体数组 |
| `world.find(name)` | 返回首个同名实体，未找到时为 `nil` |
| `world.components()` | 返回已经注册的组件名称数组 |
| `world.components(entity)` | 返回实体实际拥有的已注册组件名称数组 |
| `world.has(entity, component)` | 判断实体是否拥有指定组件 |
| `world.get(entity, component)` | 返回组件的拥有型反射快照 |
| `world.set(entity, component, value)` | 校验并暂存整个组件快照 |
| `world.position(entity)` | 返回世界位置 `x, y, z`，单位为米 |
| `world.rotation(entity)` | 返回欧拉角 `x, y, z`，单位为度 |
| `world.scale(entity)` | 返回局部轴缩放 `x, y, z` |
| `world.set_position(entity, x, y, z)` | 暂存世界位置 |
| `world.set_rotation(entity, x, y, z)` | 暂存欧拉角 |
| `world.set_scale(entity, x, y, z)` | 暂存局部轴缩放 |

位姿便捷函数要求目标仍然有效且具有 `Transform`。写入必须是有限数值，缩放每轴绝对值不能小于 `0.0001`。同一次回调的后续读取会看见自己的暂存写入。整个回调成功才将全部组件修改发布到 ECS 并推进 World 版本；执行出错时丢弃该回调的全部暂存组件。

`reflect` 提供类型、字段、属性、构造函数、继承、枚举和函数的统一查询与调用。引擎类型和自定义类型通过同一套 C++ 注册机制开放，完整接口及扩展示例见 [`Reflection.md`](Reflection.md)。例如修改世界中的所有光源：

```lua
function update(self, dt)
    for _, entity in ipairs(world.query('Light')) do
        local light = world.get(entity, 'Light')
        light.intensity = 20
        light.color.x = 0.5
        world.set(entity, 'Light', light)
    end
end
```

本版本不提供 Lua 创建或删除实体、纹理编辑、文件访问或网络访问 API。`MeshRenderer.materialIndex` 可以选择世界中已有的有效材质槽，网格资源身份和几何范围保持只读。可使用基本语言能力及 `math`、`string`、`table` 标准库；不开放 `io`、`os`、`package`、`debug`、文件加载器及动态 `load`。脚本侧 `pcall` 和 `xpcall` 不开放，防止反复捕获指令预算异常；错误恢复由 C++ 宿主统一处理。

### 错误与执行预算

每个虚拟机的 Lua 分配器最多使用 8 MiB。初始化和每次更新分别限制约 100,000 条 Lua 指令。这些预算不计入反射 C++ 对象的原生分配和被调用的原生函数耗时，因此注册函数应提供有界操作。初始化可以查询类型和构造普通值，但不能访问或修改 World。无效源码保留上一版可用虚拟机；运行时异常或超时只暂停出错实例，并显示中文包装的 Lua 原始诊断，其他实体脚本继续执行。修正源码并成功重新编译后恢复该实例。

这里的隔离和预算用于防止一般编辑错误影响应用，不是运行不可信代码的操作系统级安全沙箱。

## 对象、材质和纹理的所有权

`World::Resources()` 显式拥有 CPU 材质、网格和图像。实体通过网格资源句柄、几何范围和材质槽引用资源；`MaterialResource` 保存光学参数与借用的纹理采样视图。图像像素、UV 变换和采样器配置保持导入时的资源绑定关系。

替换一个对象的材质只修改该对象的材质槽，不重写原始网格，也不修改其他对象的分配。多个对象引用同一材质时，编辑该共享材质的参数或 HLSL 源码会同时影响这些对象。表面脚本按实际命中的材质槽分发，不存在一个覆盖全场景的隐式全局材质。

## 验证入口

`lua_script_tests` 验证实际 ECS 位姿更新、暂停、带 BOM 源码、编译失败保留旧版本、回调原子提交、错误隔离、无限循环预算以及过期实体标识。

`reflection_tests` 验证通用 C++ 元数据、构造、继承、重载和独立上下文。`lua_binding_tests` 验证自定义类、容器、引用实参和严格转换。`lua_reflection_tests` 验证真实世界中的类型查询、组件发现、多组件写入和错误回滚。

`material_script_tests` 使用实际 D3D12 设备验证完整管线替换、无效 HLSL 保留旧管线、纯光栅化排列、移除脚本、后台编译期间继续绘制，以及同一渲染器中已有对象 GBuffer 像素的变化和失败后的保持。此项需要 GPU。`editor_history_tests` 验证脚本源码、文件路径、组件存在状态和启用标志的撤销及重做。
