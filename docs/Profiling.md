# 性能录制

性能窗口采用显式录制会话。默认处于未录制状态；此时 Renderer 不向 RDG 执行器传入 GraphProfiler，不读取逐 Pass/屏障的 CPU 时钟，也不插入 GPU timestamp 或查询解析命令。渲染面板的帧耗时显示和 RDG 流程列表独立保留。

## 录制和回看

- 点击“开始录制”建立新会话，并清除上一会话的结果；停止期间残留的最后一帧不会混入新会话。
- 录制中接收每帧已经完成 GPU 栅栏的样本，时间线跟随最新帧；同一个样本重复送达不会增加帧数。
- 点击“停止录制”立即关闭后续帧的采集，已经录制的帧保留在内存。使用帧滑块查看任意一帧，再切换 CPU/GPU、缩放、搜索或选择阶段。
- 每个会话最多保存 600 帧，达到上限自动停止。此限制保留完整会话，不采用不断覆盖旧帧的实时环形缓存。
- “清空”释放录制数据。成功替换世界会停止旧会话并保留旧结果；再次录制时建立新的会话。

录制状态属于编辑器工具，不进入对象默认值、撤销历史或 Protobuf 世界文档。当前没有会话文件导出、跨运行加载或 Unreal Insights 格式兼容。

## 数据含义

GPU 使用 direct queue 的原生 timestamp 和实际队列频率，查询仅在对应提交的 fence 完成后读回。每个阶段保留真实起止位置及阶段之间的空隙，不将持续时间相加伪造事件位置。

CPU 总量包括图编译和命令录制，逐 Pass 包含其执行回调及前置屏障，另保留屏障数量和 CPU 提交耗时。CPU 与 GPU 使用各自时间原点，两条轴不能相加或据此推断跨处理器重叠。

当前录制覆盖主渲染图，包括主图内的编辑器覆盖层；它不是完整引擎线程采样器。Gameplay/Lua 更新、界面搭建、后台 HLSL 编译、图外等待和 Present，以及独立平台窗口队列的完整 GPU 时长不包含在主图 GPU 总量中。当前渲染器仍同步提交等待，不能把录制结果当作多帧并行性能基准。

## 实现入口

- [`EditorProfilerUi`](../Source/Editor/EngineEditor/Public/editor/EditorProfilerUi.h) 拥有会话、帧预算和回看选择，空闲时不复制运行时 Pass 数据。
- [`Dx12Renderer`](../Source/Editor/EngineEditor/Private/Dx12Renderer.cpp) 在界面更新后将录制请求交给渲染器，同一帧开始或停止采集。
- [`FrameRenderer`](../Source/Runtime/Renderer/Private/Frame/FrameRenderer.cpp) 默认关闭采集，仅在显式请求时接入原生分析器。
- [`GraphExecutor`](../Source/Runtime/DirectX12RHI/Private/GraphExecutor.cpp) 跳过未启用分析器时的 CPU 时钟及 GPU 查询；[`GraphProfiler`](../Source/Runtime/DirectX12RHI/Private/GraphProfiler.cpp) 负责原生查询和 fence 校验。

## 验证入口

`editor_profiler` 使用真实 ImGui 控件验证空闲拒绝、开始/停止、重复帧和未完成帧拒绝、逐帧回看、清空、重录及 600 帧自动停止，并保留时间条位置、悬停、缩放、滚动和搜索回归。

`raster_pipeline` 在真实 D3D12 上验证默认无样本、显式开启后产生完成时间戳、停止后连续渲染不覆盖结果，以及重新开启后采集序号继续增长。`graph_profiler` 独立验证 GPU 负载、查询范围和完成栅栏。实际执行结果以 [`Validation.md`](Validation.md) 为准。
