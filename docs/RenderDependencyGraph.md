# Render Dependency Graph 资源契约

RDG 统一描述资源读写、初始化、执行顺序、状态转换和瞬时资源寿命。固定渲染阶段只提供建图入口；编辑器的 Pass 列表来自实际编译计划。阶段扩展的公共接口见 [渲染管线扩展](RenderPipelineExtensions.md)。

## 资源声明与访问模式

平台无关的 [`RenderGraph`](../Source/Runtime/RenderCore/Public/render/RenderGraph.h) 提供两种资源入口：

- `Import(name, initialState, initialized, preserveWrites)` 引入图外资源，不创建物理分配。默认内容已经初始化，且图外可观察的写入需要保留；特殊调用可显式关闭这些默认条件。
- `Create(name, ResourceDescriptor)` 声明图拥有的瞬时纹理或缓冲区。描述包含类型、尺寸、格式、结构步长、能力标记、优化清除值和默认堆或上传堆类型。声明时不分配显存，新逻辑资源的内容尚未初始化。

每个 Pass 都要声明资源以及所需状态，访问模式具有以下含义：

| 访问模式 | 内容契约 | 对先前值的要求 |
| --- | --- | --- |
| `Read` | 只读取已有内容 | 必须已初始化，并保留对应生产者 |
| `Write` | 完整覆盖资源内容 | 不消费先前值，可以裁掉被覆盖的无用生产者 |
| `ReadWrite` | 读取后原地修改内容 | 必须已初始化，并保留先前生产者 |

局部写入、累加和依赖旧值的 UAV 操作不能冒充完整覆盖。显式声明读取也不能用额外的排序边替代。

## 保活与排序

编译器将“后继需要前驱产生的值”和“两个已保留操作不能颠倒”分开处理：

| 依赖来源 | 限制执行顺序 | 后继存活时保留前驱 |
| --- | --- | --- |
| RAW，以及 `ReadWrite` 对旧值的读取 | 是 | 是 |
| WAR、WAW | 是 | 否 |
| `AddDependency(before, after)` | 是 | 是 |
| `AddOrdering(before, after)` | 是 | 否 |

`AddDependency` 用于资源声明之外确实存在的生产者关系。`AddOrdering` 只参与拓扑排序，适用于覆盖层需要排在其他存活工作之后等情况；它不会让无用 Pass 因覆盖层存活而恢复执行。两者都不授予资源访问权限。

所有 Pass 声明和显式边都会先验证，包括资源及 Pass 编号、初始化读取、重复或冲突访问和环路。环路检查覆盖完整声明图，被裁掉的节点也不能隐藏错误。随后从导出值、外部可观察写入和显式副作用根传播值依赖，裁掉无用工作，并按稳定拓扑顺序执行；同样可执行的节点按原始声明编号选择。

`Lifetimes()` 记录保留计划中首次和末次使用位置，`Allocations()` 记录需要的物理槽。默认堆资源只在完整描述相同且寿命不重叠时共享槽；同一 Pass 同时访问的资源不能共享存储。被裁掉资源的分配槽为 `InvalidGraphIndex`。导出瞬时资源会将寿命延长至最终状态转换结束。

## 绑定与依赖同源

DX12 Pass 通过公共 [`PassParameters`](../Source/Runtime/DirectX12RHI/Public/dx12/PassParameters.h) 同时建立绑定与访问声明：

- `Srv` 声明只读输入及其状态，`Uav` 声明完整写入或原地读写，`Constants` 声明常量缓冲读取。
- `Access` 为复制、附件等不使用上述着色器槽位的操作补充资源访问。
- `AddPass` 从同一组参数生成 RDG 的 `Use`，并按值保存参数对象，避免建图栈上的参数对象逃逸。

建图时会检查每个 `GpuResource::graphOwner`，不能把另一张图中数值相同的 `graphId` 当成有效资源。执行回调之前，参数会再次检查所属图，调用 `RenderGraph::AssertAccess` 验证当前活动 Pass 的访问模式和状态，并确认原生对象已经驻留。Renderer 的绑定路径也使用这组参数，只有本 Pass 声明的槽位会绑定实际资源，其余槽位使用空描述符。

原生资源只能在对应 Pass 的执行回调中解析，不能在建图时缓存瞬时资源的原生对象或描述符。该检查覆盖参数接口的调用契约；原始 DX12 命令列表仍由调用者负责，接口不提供隔离任意原生命令的沙箱。

## 逐 Pass 参数上传

[`GraphExecutor::UploadParameters`](../Source/Runtime/DirectX12RHI/Private/GraphExecutor.cpp) 接收当前 Pass 的参数字节，立即保存一份 CPU 值副本，声明按 256 字节对齐的上传堆缓冲区，并添加 `Upload.<Pass 名称>` 写入 Pass。物理分配和 `Map`、复制、`Unmap` 都在保留的上传 Pass 执行时发生。消费 Pass 通过 `Constants` 或显式读取建立 RAW 依赖；如果消费链被裁掉，上传也不会分配和写入。

上传资源不能按同次提交中的 GPU Pass 寿命共享物理槽。CPU 会在 GPU 提交之前完成整张图的录制和所有 `Map` 写入，即使两个消费 Pass 的图内寿命不重叠，后一次写入也会覆盖 GPU 尚未读取的数据。默认堆资源仍可按不重叠寿命复用；上传堆的跨图复用必须等待先前提交的 fence 完成。

当前每个 Pass 的 CPU 参数值和上传资源相互独立，但 HLSL 仍使用公共 [`FrameConstants`](../Source/Runtime/RenderCore/Public/render/GpuTypes.h) ABI，与[着色器公共常量布局](../shaders/Private/Common/Common.hlsli)对应。算法设置函数分别填入各自参数，并未实现按 Pass 反射生成独立的 HLSL 常量布局或根签名。

## 物理分配与回收

[`GraphExecutor`](../Source/Runtime/DirectX12RHI/Private/GraphExecutor.cpp) 将逻辑资源绑定到地址稳定的 `GpuResource` 代理。建图时代理没有原生对象，首次保留使用之前才从 [`ResourcePool`](../Source/Runtime/DirectX12RHI/Public/dx12/ResourcePool.h) 获取 committed resource；末次使用后清空逻辑代理的原生对象和视图索引。

同图内的兼容默认堆资源复用同一个完整 committed resource。开始新逻辑寿命前，执行器将物理资源从真实上一状态转换至 `COMMON`，再应用编译屏障；池中残留字节不会令新逻辑值自动初始化。材质 SRV/UAV 表在 Pass 录制时创建，GBuffer 的 RTV/DSV 也在分配完成后刷新。

资源池将长期场景资源与瞬时缓存分开，自动导入仅包括长期资源。瞬时分配记录下一次提交的 fence 值，另一张图只有在实际命令列表已经提交且对应 fence 完成后才能复用；退休界限为完成值与最近实际提交值的较小者。`WaitIdle()` 单独发出的空闲信号不能释放尚未提交的上传字节；同一命令列表录制两张图并在中间等待空闲时，二者仍使用独立上传存储。当前帧使用 `SubmitAndWait()`；缓存保留到 `Reset()`，没有自动压缩、预算限制或尺寸类别淘汰。

## 帧资源与视图状态

[`GraphResources`](../Source/Runtime/Renderer/Private/Resources/GraphResources.h) 中的 `GraphTexture` 表示一张图内二维纹理，`GraphBuffer` 表示一份线性缓冲区，二者保存描述和稳定逻辑代理。[`RenderTarget`](../Source/Runtime/Renderer/Private/Resources/RenderTarget.h) 专指可作为颜色或深度附件的一张图内纹理，构造时验证附件能力；纯 UAV 纹理、reservoir 和整帧资源集合不称为 RenderTarget。

[`FrameResources`](../Source/Runtime/Renderer/Private/Resources/FrameResources.h) 按职责聚合资源，各组分别声明到当前图：

| 分组 | 管理内容 |
| --- | --- |
| `GBufferResources` | 位置、法线、基础颜色、切线、UV 五张颜色附件及硬件深度附件 |
| `LightingResources` | 直接光和当前 GI 输出纹理 |
| `ReSTIRResources` | 候选、时间复用、空间复用三份 reservoir 缓冲区 |
| `DenoiserResources` | 时间重建和空间滤波纹理 |
| `ScreenEffectResources` | 天空、遮蔽、反射、间接光以及两张次表面滤波纹理 |
| `ViewHistory` | 跨帧 GI、位置、法线、重建结果和 reservoir 外部存储 |

显示图像是长期外部资源，`presentation` 只指向本帧效果链当前 HDR 输出。关闭的算法或[屏幕效果](ScreenSpaceEffects.md)不会为其无用声明分配原生存储。效果链显式传递最终显示输入，原积分器和重建历史不反馈后处理能量。

[`ViewState`](../Source/Runtime/Renderer/Private/View/ViewState.h) 只保存 CPU 侧当前及上一帧相机、视口、帧编号、累计样本与重置状态，不拥有 GPU 常量缓冲。它提供视图参数值；光照、积分器、ReSTIR、重建、屏幕效果和后处理在各自 Pass 的实现文件中填写自己的算法参数，不经过集中参数设置模块。成功提交并等待完成后才推进 CPU 相机历史和样本计数。

## 历史轮换与诊断复制

[`ViewHistory`](../Source/Runtime/Renderer/Private/View/ViewHistory.cpp) 为 `History.GI`、`History.Position`、`History.Normal`、`History.Resolved` 和 `History.Reservoir` 持有图外资源。初始化图清零这些存储，逐帧导入；仅 ReSTIR 分支提取当前 reservoir。

`QueueExtraction` 将瞬时默认堆结果发布为下一帧历史，流程如下：

1. 建图时验证源属于当前图且为瞬时资源，目标是当前图已导入的资源池外部默认堆存储，完整物理描述兼容。源与目标各自只能出现在一项轮换请求中。
2. 添加末端读取和导出，保留源值并延长寿命；此时不填充历史代理，也不录制 `CopyResource`。
3. 执行前再次验证全部目标。只有 `RenderGraph::Execute` 的所有 Pass、寿命处理和最终屏障成功后，执行器才交换源物理分配与外部历史分配。外部历史代理保持逻辑身份，旧历史存储返回受 fence 保护的瞬时缓存。

这条路径使用默认堆物理资源的零拷贝轮换，不适用于上传堆、读回堆或不匹配描述的资源。交换发生在命令录制成功之后、`SubmitAndWait()` 之前；若随后提交失败，已经发布的历史不会回滚，CPU `ViewState::Complete` 也不会执行。调用方必须将该帧视为失败，不应假定图录制成功等于 GPU 已完成。

`GraphExecutor::Extract` 保留显式 GPU 复制语义：声明源 `COPY_SOURCE`、目标 `COPY_DEST`，录制 `CopyResource`，并导出外部目标。`FrameRenderer::RetainFrameImages()` 使用此路径按需保留 GBuffer、直接光和空间滤波快照供诊断及数值读回；正常渲染默认不创建这些快照，也不能通过快照继续访问已过期的瞬时原生对象。

## 当前边界

当前实现使用单条 direct queue、单 mip/单 sample 二维纹理及线性缓冲区，跟踪整资源状态并复用完整 committed resource。尚无异步 compute、跨队列同步、子资源独立依赖、placed heap 别名及 aliasing barrier、并行 Pass 录制、独立着色器布局反射或缓存预算淘汰。阴影 atlas 与场景资源仍有明确的长期所有者。

图编译错误发生在本图瞬时分配和 Pass 命令录制之前。Pass 录制异常会终止执行，尚未完成的图不会发布排队的历史轮换；物理存储仍由池持有。资源状态和部分录制命令没有事务回滚，调用方必须放弃失败的命令列表，不能继续提交或从失败点续录。

## 验证入口

以下入口描述现有用例覆盖内容，不代表本次文档维护已执行测试；GPU 目标需在 Windows 构建中启用 `ENCORE_ENABLE_GPU_TESTS`。运行方法与环境条件见 [验证说明](Validation.md)。

- [`core_tests`](../tests/CoreTests.cpp) 的 `graph hazards`、`graph ordering`、`graph validation`、`graph access validation` 和 `graph transient lifetimes` 覆盖值依赖、纯排序不保活、全图环路拒绝、执行期访问契约、寿命复用与裁剪。
- [`renderer_integration_tests --rdg-only`](../tests/RendererIntegrationTests.cpp) 包含真实 GPU 瞬时存储复用与读回、逐 Pass 上传隔离及无用上传裁剪、默认堆历史交换和内容读回、提取描述拒绝等用例；对应 CTest 入口为 `rdg_resources`。
- [`render_pipeline_tests`](../tests/RenderPipelineTests.cpp) 和 [`render_extension_integration_tests`](../tests/RenderExtensionIntegrationTests.cpp) 分别检查 CPU 扩展协议以及独立模块的真实 GPU 参数上传、资源依赖和读回，详见 [渲染管线扩展](RenderPipelineExtensions.md)。
