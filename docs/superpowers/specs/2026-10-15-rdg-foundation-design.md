# 内部 M1：DX12 与 RDG 基础闭环设计草案

日期：2026-10-15。状态：待用户确认。本文是本地设计过程产物，不加入 Perforce changelist。

范围修订：用户要求正式第一版必须包含全局光照、阴影和后处理。本文仅描述内部基础联调阶段，不再代表第一版交付。正式首版规格见 `2026-10-15-v1-rendering-design.md`。

## 交付目标

在 D:\Render 建立可编译、可运行的 Windows x64 C++20 引擎骨架。启动 Editor 后显示采用简单前向直接光的 Cornell Box，具有深度遮挡、红绿侧墙、白色地面 / 顶面 / 后墙及两个实体。编辑器可调相机和直接光参数，显示 RDG 的实际 Pass 和资源信息。GPU 清屏、场景绘制、ImGui 合成和截图读回都由 RDG 调度。

这是总体路线的第一个内部可运行节点。M1 画面使用可调的简单直接光与环境项，并明确标记渲染模式。正式 V1 需要在该基础上继续整合材质、空间结构、资源导入、DXR GI、阴影、体积和后处理，通过首版规格的全部验收后再交付。

## 模块与边界

| 构建目标 | 职责 | 允许的引擎依赖 |
| --- | --- | --- |
| RenderCore | 日志、错误、基础类型、数学约定、资源描述 | 无 |
| RenderPlatform | Win32 窗口、输入、计时、系统信息 | Core |
| RenderRHI | D3D12 设备、资源、队列、描述符、命令、fence、交换链封装 | Core、Platform |
| RenderGraph | Pass / Resource 描述、依赖与生命周期编译、执行计划 | Core、RHI |
| RenderData | 顶点、材质参数、相机、实例与灯光快照 | Core |
| RenderRenderer | Cornell 渲染策略、HLSL、图构建、ImGui 合成 Pass 适配 | Core、RenderData、RHI、RDG |
| RenderEditor | 应用循环、场景参数、ImGui 交互、错误界面 | Platform、RenderData、Renderer |
| RenderTests | CPU 图编译测试与可选 GPU 集成测试入口 | 测试所需目标 |

初期 Cornell 快照由示例场景构造器提供；M2 用 World / Assets 替换数据来源而不改变 Renderer 消费契约。Dear ImGui 的交互代码属于 Editor，Renderer 提供只接收 ImDrawData 和纹理句柄的合成适配器。

## RHI 与帧生命周期

- 创建 DXGI factory，枚举高性能硬件 adapter；不因远程显示适配器存在而误选虚拟显示设备。正常交互模式使用实际硬件；显式测试参数可选择 WARP 验证基础光栅。
- 以 D3D12 feature query 记录支持的 feature level、Shader Model、ray tracing tier 与后续相关能力。M1 基础光栅不要求 DXR；硬件能力只按查询结果报告。
- 使用 flip-model 交换链、独立 FrameContext、command allocator 和 fence；复用帧上下文、上传内存与描述符前等待对应 GPU 完成值。
- 资源的 CPU 对象及底层 D3D12 对象由明确所有者管理；释放和池复用以 GPU 完成值为边界。首版不做 placed-resource aliasing。
- 窗口尺寸变化先确保受影响资源不再使用，再调整交换链和深度目标；最小化时暂停图执行。设备移除记录错误与可用诊断并受控退出。
- Present 为图外平台动作。图执行前把 swapchain backbuffer 以 PRESENT 状态导入，编译后保证最后回到 PRESENT。

## RDG API 语义

建图、编译、执行三个阶段分开。建图使用类型安全的 TextureHandle / BufferHandle，包含资源 ID、代次和版本。物理资源绑定直到图编译后才解析，用户 Pass 不持有可随意提交命令的全局设备接口。

1. 资源创建得到未初始化的版本；导入资源可以声明已有有效内容、初始状态和导出目标状态。仅有外部内存并不自动代表内容有效。
2. 写入生成新版本；读取必须指定已存在的版本。同一逻辑资源的原地写入需要关联旧版本访问，编译器建立 read-after-write、write-after-read、write-after-write 约束，避免拓扑排序覆盖仍被读取的旧数据。
3. Load / Preserve 写入必须依赖有效旧内容；Clear / Discard 写入可完全覆盖。图导出的最终版本为保活根，外部写入和截图回读通过显式副作用根保活。
4. Handle 跨帧使用、读取未初始化内容、非法用途组合和环都在编译阶段返回具体错误。Pass 顺序只能影响无依赖节点的稳定排序，不能替代正确的依赖声明。
5. 首版以整资源为状态单位；Pass 内禁止同一资源同时使用冲突的只读 / 可写绑定。可以在同一个 Pass 内合法 read-modify-write 的 UAV 必须使用显式声明，并受 UAV barrier 规则约束。
6. 编译结果包含有效 Pass 次序、各 Pass barrier、资源使用区间、初始 / 最终状态、物理资源需求及调试名称；执行层按计划录制和提交。

Pass 执行上下文只提供当前 Pass 声明的资源视图。CPU 捕获数据在提交前固定，上传源数据与 GPU staging 的有效期分别管理。框架在图外创建 PSO / shader / sampler，实际上传及 GPU 使用始终通过 Pass。

## M1 渲染数据流

初始化图：UploadGeometry、UploadImGuiFont，分别声明 CopyDestination、暂存源和后续可用状态。

逐帧图：UploadFrameData -> CornellForward -> ToneMap -> ImGuiComposite -> 可选 ScreenshotReadback -> backbuffer 的 PRESENT 终态。

CornellForward 写 HDR SceneColor 和 Depth；其直接光计算只用于验证几何、法线、相机和资源绑定。ToneMap 读取 HDR 并写 swapchain，ImGuiComposite 保留已有 backbuffer 内容后合成。截图 Pass 在合成后复制最终图像至 readback，等待 fence 后由 CPU 编码保存。

场景数据、资源描述和光照输入包含明确的长度、颜色空间和坐标约定。建议内部使用米、右手坐标、Y-up、线性 HDR；投影采用 D3D 的 0..1 深度范围，并在相机实现中统一处理绕序和正反面。

## 编辑器范围

- 主视图显示实际 Cornell 渲染，基础面板包括 Scene、Camera / Light、Renderer、RDG、日志。
- 控件提供相机移动、渲染尺寸、曝光和简单直接光参数；输入在 ImGui 捕获鼠标 / 键盘时不穿透到场景相机。
- RDG 面板显示当前图的实际 Pass 顺序、资源名、用途、生命周期与 barrier 计数；数据来自编译结果。
- 错误具有清晰的 shader 文件 / 行号、HRESULT 或 Pass / Resource 名。窗口尺寸、按钮及面板使用稳定尺寸约束，验证常用桌面分辨率下无文字重叠。

## 测试与可观测性

CPU 图测试覆盖：正确拓扑序、独立节点稳定排序、无用 Pass 消除、导出和副作用根、读前写验证、陈旧句柄、循环依赖、原地更新前读者保留、读写 barrier 以及 imported backbuffer 的最终状态。

GPU 测试在硬件可用时启用 Debug Layer / InfoQueue，检查清屏和实际几何绘制的回读结果，验证非空画面、可区分的红绿区域、深度遮挡及最终输出尺寸。连续 resize / minimize / restore 后检查错误和 fence 进展；不得以窗口成功创建代替绘制验证。Debug Layer 不可用时明确记录验证缺口。

GPU timestamp 延迟回读，CPU 记录帧时间与图编译耗时。M1 先收集当前设备的基线，不预先声称达到某个帧率。截图和临时诊断脚本属于本地过程产物。

## 验收清单

- 用明确的 CMake 配置与构建命令从干净构建目录生成可执行文件。
- CPU RDG 测试通过；GPU 验证是否实际执行及使用的 adapter 均记录在结果中。
- Editor 在本机硬件显示有几何和深度的 Cornell Box；不是只清屏或显示静态图片。
- 每项 GPU 工作能在 RDG 面板或捕获的事件中找到，包括 ImGui 与读回。
- 改变相机和曝光影响真实画面；ImGui 输入捕获正常；窗口恢复和调整尺寸后继续渲染。
- 报告实际执行的测试、输出位置和已知限制，不将 M1 宣称为总体 16 项全部完成。

## 确认后的下一步

结合正式 V1 规格，分模块创建实现计划，再开始构建、实现和验证。内部基础节点完成后继续整合 GI、阴影与后处理，不在 M1 节点提前宣布第一版完成。
