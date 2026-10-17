# DX12 游戏引擎总体设计草案

日期：2026-10-15。状态：待用户确认。本文是本地设计过程产物，不加入 Perforce changelist。

用户已明确首版必须包含全局光照、阴影和后处理，Lumen 与 Nanite 可以延后。下文 M1–M8 表示内部实现阶段，不等同于版本交付；正式 V1 的范围与验收以 `2026-10-15-v1-rendering-design.md` 为准。早期的 M1 基础闭环只作为内部联调节点，不再作为首版完成标准。

## 目标与现状

从空目录 D:\Render 创建独立的 Windows x64 / DirectX 12 游戏引擎，采用《游戏引擎架构》的分层思想。引擎以 RDG 为 GPU 工作组织基础，提供光栅、硬件光追、材质、资源导入、编辑器和 Cornell Box 示例。按可运行的阶段推进，功能存在与质量达标分别验收。

本机已观察到 Windows 11 x64、NVIDIA GeForce RTX 5070、Visual Studio 2022、CMake、Windows SDK 10.0.22621.0 / 10.0.26100.0 和 SDK 附带的 DXC。尚未运行 C++ 编译或 D3D12 能力探测，因此这些信息不代表 DXR、Mesh Shader 或目标 Shader Model 已验证。WMI 的 AdapterRAM 字段不作为显存容量依据。

当前目录为空，未发现项目 AGENTS.md 或版本控制标记。设计文档只落盘；版本控制归属在纳管前确认，使用 P4 时先运行用户指定的工作区解析器，不假定当前目录属于任何 client。

## 路线比较与建议

| 路线 | 收益 | 代价 |
| --- | --- | --- |
| 自研分层运行时、DX12 RHI 和 RDG，采用成熟的外围库，推荐 | 掌握资源状态、调度、材质和渲染算法，最贴合本项目目标 | 同步、生命周期、调试与资源管理需要完整实现和验证 |
| 使用成熟 RHI 封装的 DX12 后端，自研 RDG 与渲染系统 | 更快建立渲染实验平台 | 需要接受外部 RHI 的资源和调度模型，底层自主性降低 |

建议采用第一条路线。项目初期只实现 DX12 后端，RHI 用于划分职责；其他图形 API 在出现实际需求后扩展。

## 分层与编译依赖

下列为本项目的分层设计，不宣称逐层复刻书中的全部子系统。

| 层 | 模块 | 职责 |
| --- | --- | --- |
| 0：平台与外部设施 | Windows、D3D12、DXGI、DXC、第三方库 | 操作系统、驱动接口、编译器及成熟的格式解析和数学工具 |
| 1：基础设施 | Core、Platform | 日志、错误、句柄、数学、文件、计时、内存和任务基础；窗口、输入、系统信息 |
| 2：GPU 基础 | RHI.D3D12、RDG | 设备、队列、资源、描述符、PSO、fence；图编译、依赖、资源状态、执行与诊断 |
| 3：引擎系统 | Assets、Spatial、RenderData、Renderer | 资源导入与缓存；BVH / 八叉树；渲染快照；光照、阴影、光追、体积、后处理 |
| 4：运行时与游戏框架 | World、Runtime | Entity / Component、Transform、场景、相机、更新循环、场景读写和示例行为 |
| 5：工具与应用 | Editor、CornellSample | ImGui 编辑器、调试视图、运行配置、示例场景和可执行程序入口 |

依赖约束：Core 不依赖其他引擎模块；Assets / Spatial / RenderData 不依赖 Renderer；World 通过 RenderData 产生快照；Renderer 通过 RDG / RHI 消费快照；Runtime 组合 World 与 Renderer；Editor 位于应用侧。RHI 和 RDG 不读取编辑器或游戏对象。通过独立构建目标约束这些边界。

每帧数据流：输入和编辑操作 -> World 更新 -> 不可变 RenderScene 快照 -> 可见性与渲染策略 -> RDG 声明 -> 图编译 -> GPU 执行 -> 图外平台 Present。

建议技术基线：C++20、CMake、HLSL / DXC、DirectXMath、Dear ImGui、成熟 glTF 解析库、成熟图像解析库，后续使用 meshoptimizer 辅助网格处理。依赖在引入时锁定版本并保留许可证。初期使用明确的组件存储，存在需求时再增加复杂反射、脚本、物理或音频系统。

## RDG 契约

- 所有 GPU 工作都由 RDG Pass 声明，包括初始及逐帧上传、光栅、Compute、BLAS / TLAS 构建、光追、后处理、ImGui 合成和截图回读。设备 / PSO 创建、CPU 描述符分配、交换链操作属于对应平台或 RHI 职责。
- Pass 明确声明纹理和缓冲区的读写、使用方式和子资源范围；图内使用带代次的类型安全句柄。首版允许保守的整资源状态跟踪，禁止以同一纹理的不同 mip 同时充当 SRV / UAV；Hi-Z 阶段再扩展到逐子资源跟踪。
- 编译器从导出资源和显式副作用根节点反向确定有效 Pass，建立依赖并拓扑排序；未初始化读取、失效句柄、非法读写、环和不兼容用法产生带资源名及 Pass 名的错误。写入版本与边界定义在 M1 规格中细化。
- 编译器推导 transition / UAV barrier、导入资源的初始及最终状态，计算资源生命周期。上传暂存和 CPU 捕获数据的生命期必须覆盖 GPU fence。
- M1 使用单 Direct 队列、保守状态模型和 fence 安全的资源池；后续增加 Copy / Compute 队列、跨队列依赖、并行录制和瞬态资源别名，并分别提供正确性测试。
- 图外直接提交 GPU 渲染命令视为架构违规。Pass 实现只能通过当前 Pass 的受限执行上下文取得已声明资源。

## 空间结构与剔除

- CPU BVH：对三角形和实例建立可验证的 SAH 层次，服务于拾取、精确射线查询与算法基准。支持刚体实例变换和必要的 refit / rebuild。
- Loose Octree：维护场景对象和体积的粗粒度空间分区，支持移动对象更新、视锥 / 范围候选集查询；跨节点对象的归属规则保证不漏查。
- DXR：通过 BLAS / TLAS 接入硬件光追；驱动构建的硬件加速结构与自研 CPU BVH 分别负责不同用途。
- 剔除按顺序发展为距离、CPU 视锥、八叉树层级、背面、GPU Hi-Z 遮挡、meshlet 法线锥与屏幕误差选择。每种算法可单独启停并显示候选 / 保留 / 剔除计数。
- 相机不可见的对象仍可能出现在阴影、镜面或间接光中；主视图可见列表不得直接代替阴影集合或 DXR TLAS 实例集合。遮挡剔除使用保守边界、相机变化历史失效和双面材质例外。

## 材质、光照与两条渲染管线

材质由 Material 定义、MaterialInstance 参数和纹理组成。CPU 与 HLSL 共享明确的字段布局和语义；统一资源句柄、颜色空间、单位、法线和 UV 约定。材质覆盖 baseColor、metallic、roughness、normal、emission、IOR、transmission，以及逐步增加的 clearcoat、sheen 和 anisotropy。镜面和折射具有独立的 delta 分支，避免把 roughness = 0 当作普通 GGX 分布数值计算。

Disney Principled BRDF / BSDF 描述局部反射与透射；GI 求解器采用路径追踪与后续实时 GI。镜面与玻璃使用 Fresnel、折射、全内反射及必要的辐射度传输修正。微表面采用 GGX / Smith，并使 Evaluate、Sample 和 PDF 语义一致。

| 管线 | 行为 |
| --- | --- |
| Raster | 深度、GBuffer、阴影、直接光、DXR 蒙特卡洛间接光、透明 / 透射、体积、时域重建、色调映射；内部 M1 从简单前向直接光开始，正式 V1 必须整合 GI、阴影和后处理 |
| Path Tracing | 真实 DXR 射线与三角形求交、蒙特卡洛路径积分、next-event estimation、MIS、重要性采样、Russian roulette、渐进累计；作为 GI 和材质参考 |

两条管线共享场景、资源和材质语义，通过 RDG 构建独立 Pass 集。改变相机、物体、光源、材质或积分参数后，路径累计必须失效。降噪结果与原始累计分别保留，算法比较以原始 HDR 数据为准。

正式 V1 的混合管线从 GBuffer 的表面出发，用 DXR 追踪可配置深度的间接路径；GI 与直接光分开输出，并以 NRD REBLUR 对不透明表面信号执行时空降噪。NRD 的 Compute 调度逐项映射为 RDG Pass。Path Tracing 模式提供全场景渐进积分参考。两种模式均提供多次反射和离屏几何贡献，不能用常量环境光或 AO 代替 GI。

光源包括点光、聚光、方向光、矩形面光、圆盘面光及发光三角形网格；矩形光是面光源的一种特化。有限面光源按面积采样并转换为立体角 PDF；点光等 delta 光源使用独立概率语义，防止错误 MIS 或距离衰减。

阴影选项中的 VSM 明确表示 Variance Shadow Maps，不等同于 UE 的 Virtual Shadow Maps。PCF、PCSS、VSM 先覆盖有定义的聚光 / 方向光 shadow map；点光通过 cube shadow map 及对应过滤扩展。有限面光源的正确软阴影由采样可见性求解；纯光栅模式若采用代表光近似，需要明确标识，不能与完整面光积分混称。

## 体渲染

统一 Medium 数据中的吸收、散射、密度、各向异性和边界。先实现均匀介质、Beer-Lambert 透射和 Henyey-Greenstein 相函数，再支持 3D 密度纹理。实时路径采用 froxel / ray marching 的单次散射与时域积累，路径追踪采用距离采样和介质事件。介质与玻璃边界需要正确处理内部 / 外部切换；高阶多重散射作为独立扩展验收。

## 编辑器与 Blender 导入

- ImGui 编辑器提供 Scene / Hierarchy、Viewport、Inspector、Assets、Renderer Settings、RDG Pass 列表、Profiler、日志和空间结构可视化。选择与编辑通过 World API 更新场景；Renderer 只读取快照。
- 场景版本化保存实体稳定 ID、组件、材质实例和资源引用；资源引用使用 asset ID / 相对工程引用，不把开发机绝对路径写入交付内容。
- Blender 首期走 glTF 2.0 / GLB 导出 -> 引擎导入，覆盖静态三角网格、层级、变换、UV、法线 / 切线、纹理和 metallic-roughness 材质。单位、坐标和绕序在导入边界统一转换。
- 扩展阶段映射受支持的 transmission / IOR / clearcoat 等 glTF 扩展；不支持的材质节点或扩展必须报告。Blender 任意节点图通过烘焙纹理进入管线。骨骼动画、直接读取 .blend 和 FBX 不作为首期导入承诺。

## Lumen 与 Nanite 研究阶段

参考 UE 5.8 公开技术资料，分别实现本引擎的实时 GI / 反射系统和虚拟几何系统。是否实现某项能力由代码路径、调试视图和测试证明；初版不宣称与 UE 5.8 全功能或性能等价。

实时 GI 分阶段接入屏幕追踪、DXR 离屏追踪、Radiance Cache、Surface Cache / Cards、Screen Probe Gather、空间 / 时域过滤和反射；距离场软件追踪作为独立扩展。验收包含无烘焙的色溢、遮挡、移动光源更新、离屏几何贡献和历史失效；对照路径追踪记录误差与 GPU 成本。

虚拟几何先实现静态不透明网格的层级 cluster、屏幕误差 LOD、GPU 遮挡剔除、间接绘制及诊断；随后实现带父级保底的分页流式、预算内驻留和缺页反馈，再逐步完善可见性缓冲与小三角形软件光栅。普通 meshlet 绘制只能标注为前置阶段。LOD 生成需要保持共享边界或提供等效无裂缝约束；验收包含相邻 cluster 的裂缝、LOD 转换、未驻留页保底和内存预算。DXR 初期使用独立三角形代理，跟踪与可见几何的偏差。

## 分阶段交付与验收

| 阶段 | 交付 | 核心验收 |
| --- | --- | --- |
| M1：RDG 基础闭环 | 分层构建、Win32 / DX12、图编译、简单前向 Cornell Box、基础 ImGui、回读和测试 | 编译运行；resize / minimize 正常；所有 GPU 工作经 RDG；图编译和 barrier 行为有测试 |
| M2：运行时与资产 | World、场景读写、材质实例、glTF / GLB、BVH、八叉树、CPU 剔除 | Blender 导出资产可导入；空间查询对照暴力结果；场景保存后重开一致 |
| M3：实时光照与阴影 | GBuffer、PBR 直接光、点 / 聚 / 方向光、PCF / PCSS / VSM、调试视图 | 同一固定场景切换阴影策略；查看光源、深度和矩数据；记录漏光与 acne 情况 |
| M4：光追与参考积分 | DXR、Principled BSDF、GGX、镜面、BTDF、面光、MIS、渐进累计 | 真实 DXR dispatch；Cornell 色溢 / 反射 / 折射；多随机种子下采样收敛统计 |
| M5：参与介质 | 均匀及纹理密度体积、实时体积光、路径追踪介质 | 零密度恒等、Beer-Lambert 数值对照、边界切换正确、固定场景体积光柱 |
| M6：GPU 可见性 | Hi-Z、间接绘制、cluster / meshlet 剔除、性能统计 | 与禁用剔除的画面对照；相机突变无错误剔除；实际节省 GPU 工作可测 |
| M7：实时 GI 研究实现 | Probe / Cache、屏幕与硬件追踪、GI / 反射与历史重建 | 动态无烘焙 GI、离屏贡献、路径参考误差和帧成本报告 |
| M8：虚拟几何研究实现 | 层级 cluster、无裂缝 LOD、GPU 可见性、分页流式和驻留管理 | 高模压力场景、缺页保底、预算受控、LOD / streaming 调试视图和统计 |

正式 V1 合并 M1–M5 与 M6 的基础可见性能力；M7、M8 对应后续 Lumen / Nanite 研究阶段。V1 的后处理交付包括 HDR、手动 / 自动曝光、Bloom、TAA、ACES 拟合色调映射与输出 Gamma；GI、阴影和后处理均为正式首版的强制验收项。

基础 Cornell Box 从 M1 开始贯穿每一阶段。最终示例包含红绿墙、白色漫反射面、两块不同高度的实体、顶面矩形光、镜面物体、玻璃物体及可切换的局部介质。额外提供高几何复杂度实例场景检验八叉树、GPU 剔除和虚拟几何；小型 Cornell Box 本身无法证明这些系统的性能收益。

## 验证、失败处理与质量边界

- Core / RDG / Spatial 使用不依赖 GPU 的自动化测试；D3D12 路径使用实际设备、Debug Layer、InfoQueue、必要的 GPU-based validation 和 GPU 回读。
- 图测试覆盖依赖、无用 Pass、非法读写、状态、UAV barrier、资源导出和 GPU 完成前不得复用。后续分别覆盖多队列、别名和逐子资源状态。
- 采样与材质验证覆盖 PDF 归一化、反射 / 透射分支、全内反射和数值有限性。蒙特卡洛测试使用多个随机种子及置信范围，避免只依赖单张截图。
- 无 DXR 时保持 Raster 可用并显示能力诊断；设备移除输出 DXGI / DRED 诊断后受控退出；零尺寸窗口暂停渲染。资源损坏、扩展不支持、着色器编译失败都显示具体错误。
- 性能先测基线，再定义复杂场景预算。实时 Raster 与渐进 Path Tracing 分别报告，不把后者宣传为已经达到实时帧率。
- 每阶段记录实际执行的命令和结果；本文所有验收均为计划，当前尚未实现或运行引擎。

## 公开参考

1. Jason Gregory 的分层说明及工具期 / 运行时对象模型：https://www.gameenginebook.com/resources/GEA2_GameObjectSystems.pdf
2. Disney 的 BRDF 原始说明：https://disneyanimation.com/publications/physically-based-shading-at-disney/
3. Epic Lumen 技术说明：https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
4. Epic Nanite 技术说明：https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
5. UE 5.8 发布说明入口：https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes
6. NRD API、输入信号和原生引擎集成说明：https://github.com/NVIDIA-RTX/NRD
7. 蒙特卡洛路径积分与 MIS：https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer

以上资料用于技术研究与设计依据；代码或资源的使用需在实际引入时检查对应许可证。
