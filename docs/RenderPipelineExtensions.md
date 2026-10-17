# 渲染管线扩展

Renderer 通过公共 [`RenderPipeline`](../Source/Runtime/Renderer/Public/render/RenderPipeline.h) 提供固定阶段、阶段前后入口和递归子 Pass。外部模块链接 `renderer`，使用公共 `render/`、`dx12/` 头文件即可注册工作，无需修改 Renderer 实现或包含 `Renderer/Private`。资源访问与参数上传遵循 [RDG 资源契约](RenderDependencyGraph.md)。

## 固定帧流程

[`FrameRenderer`](../Source/Runtime/Renderer/Private/Frame/FrameRenderer.cpp) 创建本帧 RDG，声明帧资源，并注入真实 RHI 上下文；[`DeferredRender`](../Source/Runtime/Renderer/Private/Deferred/DeferredRenderer.cpp) 按以下顺序调用阶段。阶段始终提供扩展入口，具体内置分支仍受有效渲染设置控制。

| 阶段 | 内置职责 |
| --- | --- |
| `Prepare` | 场景缓冲上传及算法执行前的资源准备 |
| `Shadows` | 启用时生成阴影图及阴影滤波 |
| `BasePass` | 生成可见表面属性和深度 |
| `DirectLighting` | 独立直接光照求值；完整路径追踪分支由 GI 入口统一处理 |
| `GlobalIllumination` | 按有效配置建立路径追踪、SSGI 或 ReSTIR GI 分支 |
| `Denoise` | 时间重建、空间降噪并发布 HDR 颜色 |
| `ScreenEffects` | 消费当前 HDR，建立启用的天空与屏幕效果链 |
| `PostProcess` | 消费效果链 HDR，进行体积、泛光、显示变换及显示图像输出 |
| `History` | 排队下一帧历史轮换，并在启用诊断时建立快照复制 |
| `Overlay` | 宿主提供的最终编辑器或运行时覆盖层 |

单次 `ExecuteStage(stage, context, builtinSetup)` 的展开顺序为：

1. 按注册顺序执行该阶段的 `Before` 节点，每个节点先调用自己的建图回调，再递归执行子节点。
2. 调用内置阶段的 `builtinSetup`。
3. 按注册顺序递归执行内置阶段根节点的子节点。
4. 按注册顺序递归执行该阶段的 `After` 节点。

例如，在 `PostProcess` 注册 `Example.Copy`，再注册其子节点 `Readback`，回调路径依次为 `PostProcess/Example.Copy`、`PostProcess/Example.Copy/Readback`。`PassPath()` 提供当前路径供 RDG 调试命名；调用者应在建图时复制名称，不能延迟借用上下文中的字符串。

这些顺序是 CPU 建图回调顺序，不会自动生成阶段之间或父子 RDG Pass 之间的依赖。实际 GPU 执行由资源读写、`AddDependency` 或 `AddOrdering` 决定；无用工作仍会被裁掉。内置覆盖层通过纯排序边排在此前已建图的存活工作之后，这不会保活无用 Pass，也不涵盖之后才注册到图中的 `Overlay.After` 工作。

## 公共接口

| 接口 | 用途 |
| --- | --- |
| `GetStagePass(stage)` | 取得不可删除的内置阶段根节点，作为子节点父句柄 |
| `RegisterPass(stage, location, name, setup)` | 在阶段 `Before` 或 `After` 注册扩展节点，返回句柄 |
| `RegisterSubpass(parent, name, setup)` | 在有效父节点下注册子节点，可以继续递归注册 |
| `Unregister(handle)` | 递归删除一个扩展节点及其全部后代，影响之后的建图 |
| `IRenderExtension::RegisterPasses(pipeline)` | 独立模块可以实现的注册协议，模块自行保存和注销句柄 |

句柄包含唯一管线身份和单调增长的节点编号，删除后不复用，管线对象在同一地址重建也不会接受旧身份。同一管线的已删除句柄再次注销返回 `false`；空句柄、其他管线的句柄和试图注销内置根节点会抛出异常。无效阶段、无效位置、空节点名、空回调或已失效的父节点同样会被拒绝。

所有注册、注销和阶段执行都限定在同一建图线程。阶段执行期间禁止修改该管线，也禁止该管线重入或用同一上下文嵌套执行另一条管线。回调异常向宿主传播，执行标志和路径由作用域清理恢复；已经添加到 RDG 的节点不回滚，宿主应放弃失败的帧图。

编辑器宿主通过公共 [`Dx12Renderer::Pipeline()`](../Source/Editor/EngineEditor/Public/render/Dx12Renderer.h) 暴露管线；该管线由宿主持有，在重新加载场景并替换内部 `FrameRenderer` 时继续使用。内部 `FrameRenderer::UsePipeline()` 由宿主注入这份管线，外部模块本身无需依赖这一私有接口。

## 帧上下文与资源黑板

`RenderPipelineContext` 借用当前 `RenderGraph`、像素宽高和只读有效 `RenderSettings`。CPU 建图用例只需这些数据；真实帧调用 `AttachRhi`，之后可通过 `Executor()`、`Device()`、`Descriptors()` 访问公共 DX12 接口。未连接 RHI 时访问这些必需接口会明确抛错，不使用伪造设备引用。

资源黑板通过 `Publish`、`Find`、`Require` 访问：发布可替换同名条目，`Find` 对缺失键返回空指针，`Require` 对缺失键抛出异常。它不拥有资源，不自动导入资源，也不自动创建 RDG 依赖；模块仍须用 `PassParameters` 声明访问。

固定流程发布以下名称：

| 名称 | 发布时机与内容 |
| --- | --- |
| `Scene.Position`、`Scene.Normal`、`Scene.BaseColor`、`Scene.Depth` | 固定流程入口发布当前图的 GBuffer 逻辑代理，内容由 `BasePass` 产生 |
| `Lighting.Direct`、`Lighting.GI` | 固定流程入口发布对应逻辑代理，内容由选中的光照分支产生 |
| `Post.Display` | 固定流程入口发布长期显示图像，当前帧颜色由 `PostProcess` 写入 |
| `Scene.Color` | `Denoise` 内置入口完成建图后发布空间滤波 HDR，`ScreenEffects` 完成建图后更新为效果链 HDR |

名称已经存在不代表内容在该阶段已经初始化，也不保证对应可选分支一定执行。扩展应选择生产者之后的入口，并遵守具体资源格式与访问要求。黑板代理必须覆盖整次建图和执行寿命，每帧重新查询；场景替换后不能继续持有上一份帧资源指针。

`ScreenEffects` 和 `PostProcess` 的内置入口会重新读取 `Scene.Color`。上游扩展可以创建并声明新 HDR 输出，再发布到这个键，让后续固定入口消费它；发布其他键不会自动改写所有 Renderer 内部资源引用。扩展不通过黑板访问私有 `FrameResources` 或 `ViewHistory`。

## 向 RDG 添加工作

阶段回调只描述工作，不直接录制 GPU 命令。通常先从黑板取得输入，通过执行器显式导入模块自有资源，或创建当前图的瞬时资源；然后准备参数值，调用 `UploadParameters`，并使用 `PassParameters` 注册实际执行回调。

`PassParameters` 中的资源绑定同时生成图依赖，`AddPass` 按值保存参数并在执行时校验图身份、`AssertAccess` 权限和资源驻留状态。复制或清除等原生命令也要先声明 `Access`，然后放进 `AddPass` 的执行回调。未导入资源、跨图代理以及在活动 Pass 之外解析瞬时原生资源都不属于有效调用。

建图上下文和局部参数只在当前调用期间有效。延迟回调应按值保存 CPU 参数，捕获寿命足够长的资源、设备或共享所有权数据，不能捕获临时栈对象和 `RenderPipelineContext&`。`UploadParameters` 已复制输入字节，可以安全接收当前建图回调中的临时参数数组。

每个 Pass 的上传资源独立，但内置着色器仍共享 `FrameConstants` 的 HLSL ABI；公共扩展接口并未提供自动反射、独立常量布局生成或通用材质绑定器。模块可以用公共 RHI 接口管理自己需要的原生管线和绑定，并对其命令正确性负责。

## 独立模块示例

[`ExampleRenderExtension`](../tests/modules/ExampleRenderExtension.h) 及其[实现](../tests/modules/ExampleRenderExtension.cpp) 编译为独立静态目标 `example_render_extension`，公开链接 `renderer`。模块仅包含公共渲染和 DX12 头文件，其目标没有 `Source/Runtime/Renderer/Private` 包含路径。

示例构造函数接收宿主持有的 `Dx12Device&`，自行创建默认堆输出缓冲和读回缓冲。宿主通过 `IRenderExtension&` 调用 `RegisterPasses(pipeline)`，模块建立 `PostProcess.After` 父节点和一个子节点：

- 父节点 `Example.Copy` 通过 `Require("Post.Display")` 声明显示结果依赖，重新导入模块缓冲，独立上传包含视图尺寸的四个整数，并在 RDG 回调中复制到自有输出。
- 子节点 `Readback` 独立上传另一组四个整数，读取父输出，将两组数据合并复制到自有读回缓冲并导出。父子执行顺序由这条真实资源依赖保证。

该示例读取显示资源仅用于建立依赖，不改写显示颜色。`Readback()` 必须在宿主提交并确认 GPU 完成之后调用。示例的两组上传及读回数据能够区分父子参数，避免只检查注册标记而未验证实际执行。

[`RenderExtensionIntegrationTests`](../tests/RenderExtensionIntegrationTests.cpp) 是宿主测试，负责隐藏窗口、真实设备、交换链和 `FrameRenderer`，因此只有宿主目标 `render_extension_integration_tests` 获得 Renderer 私有包含路径。这个边界允许测试完整渲染流程，同时保留外部模块仅依赖公共 API 的编译约束。

## 注销与卸载边界

模块自行实现注销，例如示例的 `UnregisterPasses(pipeline)`；注销父句柄会递归移除子节点。`IRenderExtension` 没有自动注销器，销毁模块对象之前必须先处理其注册。静态模块和动态模块都要遵守捕获对象的寿命要求。

注销只阻止后续阶段建图，不删除已经进入 RDG 的回调，也不等待 GPU。卸载包含回调代码的动态库或释放模块资源前，宿主需要停止添加新帧，在阶段执行之外注销节点，完成或放弃所有已建图回调，等待相关 GPU 提交完成，并销毁仍保存模块回调的图。仅销毁注册对象不足以证明动态库代码可以卸载。

当前帧宿主使用 `SubmitAndWait()`，可在 `Draw` 返回后注销并释放已完成帧所用资源；这不替未来异步宿主提供自动等待或模块管理。接口没有动态库装载器、跨线程注册队列或并行录制协议。

## 验证入口

以下列出现有测试设计及入口，不代表本次文档维护已运行或通过测试。独立 GPU 示例及宿主测试目标需在 Windows 构建中启用 `ENCORE_ENABLE_GPU_TESTS`；构建和 GPU 环境要求见 [验证说明](Validation.md)。

- [`render_pipeline_tests`](../tests/RenderPipelineTests.cpp)，CTest 名称 `render_pipeline`：不创建 DX12 设备，检查固定阶段前后顺序、递归子节点、真实跨阶段 RDG 数据传递、递归注销、旧身份与跨管线拒绝、异常恢复、重入拒绝、资源黑板和延迟回调数据寿命。
- [`render_extension_integration_tests`](../tests/RenderExtensionIntegrationTests.cpp)，CTest 名称 `render_extension_integration`，标记 `GPU`：在隐藏窗口中运行完整帧，检查独立模块父子 Pass 及两次上传的实际编译顺序、最终覆盖层顺序、连续帧 GPU 数值读回、跨图身份拒绝，以及注销后下一帧不再包含扩展和上传。
- [`core_tests`](../tests/CoreTests.cpp) 和 `renderer_integration_tests --rdg-only` 检查底层访问、排序、参数上传和历史轮换契约，见 [RDG 验证入口](RenderDependencyGraph.md#验证入口)。
