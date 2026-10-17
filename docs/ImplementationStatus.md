# 需求实现状态

核对日期：2026-10-17。本表依据当前源码、正式配置和测试入口，不依据聊天长度或历史计划的勾选状态。实现存在、自动测试通过和达到完整产品要求是不同结论；本轮执行结果统一记录在 [验证记录](Validation.md)。

## 已接入的主干

| 用户需求 | 当前实现与证据 |
| --- | --- |
| DirectX 12、分层模块、UE 风格目录、细分算法与 PImpl | Core、Reflection、RenderCore、Gameplay、AssetImport、DirectX12RHI、Renderer、Editor 分离；宿主只组合模块，[架构](Architecture.md)。 |
| 真正的渲染依赖图 | 资源读写声明、依赖和裁剪、状态/UAV 屏障、瞬时寿命、延迟分配、兼容存储复用、逐 Pass 参数上传、外部资源及零拷贝历史提取，[RDG 契约](RenderDependencyGraph.md)。 |
| Godot 式资源与 ECS | Texture/Mesh/Material 继承 Resource，World 显式拥有，非拥有句柄不做引用计数；EnTT 组件和渲染快照，[资源](Resources.md)。 |
| Disney 材质、镜面、BTDF、微表面与蒙特卡洛 | 表面 BSDF 的评估、混合 PDF、采样、GGX/GTR1、反射/透射和薄表面分支，[材质说明](Materials/DisneySurface.md)。Disney 是表面散射模型，本身不是 GI 算法。 |
| 三种 GI 可选、ReSTIR 两种求交、DXR 路径追踪 | PT、SSGI、ReSTIR 分支；PT/ReSTIR 可用 Compute BVH 或 DXR 1.1 inline RayQuery。SSGI 的间接光本身只查询屏幕，后台选择只影响其共享直接光和阴影。 |
| ReSTIR 学习文档 | [学习文档](ReSTIR-GI-Learning.md)包含权重与 PDF、reservoir 数据、时空复用、可见性和偏差说明。 |
| 独立纯光栅版本 | 专用 RASTER_ONLY 排列在编译时排除 BVH/TLAS 遍历，光栅 GBuffer、LTC 面光、shadow map 和屏幕 GI 均有真实执行及读回测试，[LTC](LTC.md)。 |
| 多光源、PCF/PCSS/VSM | 方向、点、聚光、矩形、圆盘五类光源，最多 16 盏启用；三种阴影实现使用真实深度/矩图，[阴影](Shadows.md)。 |
| 体积与后处理基础 | 均匀介质、Beer-Lambert 透射、HG 单次散射、体积阴影，HDR、手动/稀疏自动曝光、Bloom、ACES 和显示编码。高级差额见下表。 |
| SSAO、SSR、SSIL、SSS、天空 | 独立效果 Pass，SSS 为屏幕扩散、天空为解析环境；均有开关与零强度/像素读回回归，[效果边界](ScreenSpaceEffects.md)。 |
| Cornell Box、保留 PBR 材质、接触漏光 | 默认配置仍包含红绿墙、清漆与金属盒体；近接触边界有两后端三 GI 的 raw HDR 外部光遮挡回归，[接触可见性](ContactVisibility.md)。 |
| ImGui 中文、UI 最后叠加 | 中文字体与面板，主图后处理后的 Editor.ImGui 覆盖层；独立窗口支持停靠、缩放、原生平台窗口，[编辑器操作](EditorControls.md)。 |
| Gizmo、UE 核心操作、Ctrl+Z、默认恢复、实时数值 | 单选透视、核心飞行/环绕/变换操作、128 步作者事务、对象/材质基线恢复，详情跟随同帧相机更新。不是完整 UE 编辑器。 |
| 内容浏览器、材质替换与实时编译、Lua | 项目文件与资源浏览、文件创建、材质拖放；HLSL 后台编译成功后原子更新，失败保留旧程序；Lua Gameplay 执行与错误隔离，[脚本](Scripting.md)。 |
| 常用物体栏与拖放创建 | 配置提供立方体、平面和五类灯光；真实拖放预览、表面落点与回退、创建时默认值、单步撤销重做及保存重开，[物体放置](ObjectPlacement.md)。 |
| 手动 Profile 录制 | 默认关闭真实采集，显式开始/停止、600 帧预算、逐帧回看和 CPU/GPU 时间线，[性能录制](Profiling.md)。覆盖主渲染图，并非整个引擎线程采样器。 |
| Protobuf 多场景层级与对象资源引用 | World → Level → Scene → Object，Object 拥有网格/材质/纹理关联，World 共享资源目录；文本/二进制加载、保存、场景选择与稳定身份，[世界文档](WorldDocuments.md)。 |
| Blender 导入、FBX、实际角色 | glTF/GLB/FBX 静态资产；茜特拉莉主体、书、杯、武器已保存为正式世界与外置纹理，[FBX](FbxImport.md)、[实际场景](CitlaliScene.md)。FBX 默认姿态烘焙不包含动画播放。 |

## 尚不能认定完成的部分

| 项目 | 已有部分 | 未完成或限制 |
| --- | --- | --- |
| BVH 与八叉树 | 中位数划分的 CPU/GPU 软件 BVH、DXR BLAS/TLAS；八叉树独立 CPU 范围查询。 | 未实现早期规格中的 SAH、Loose Octree 动态对象更新；八叉树没有接入渲染对象管理。 |
| 多种场景剔除 | 材质背面/alpha 接受规则、光栅裁剪和深度测试，RDG 无用 Pass 裁剪。 | 距离、对象视锥、八叉树层级与 GPU Hi-Z 遮挡剔除，以及对应独立开关/统计尚无实现。RDG 裁剪不能替代场景剔除。 |
| ReSTIR 完整材质路径重用 | 漫反射主导路径的有限历史和相关时空复用。 | secondary Lo 仍为漫反射近似；尖锐反射、清漆、透射和薄表面 primary 回退独立 PT，未做通用镜面/折射路径重连。 |
| 降噪与抗锯齿 | 简易时间重建、方差限制和小型空间滤波；静态路径积累。 | 没有独立 TAA、相机 jitter、动态 motion vectors 或 NRD；不能把静态积累称为完整 TAA。 |
| 体积 | 均匀介质单次散射的固定步数后处理。 | 没有早期规格中的 3D 纹理异质密度、有限体积对象及完整多次散射。 |
| 曝光与 Bloom | 16 点稀疏自动测光、固定 25 tap Bloom。 | 没有亮度直方图、随时间适应曝光或多分辨率 Bloom 金字塔。 |
| UE RDG 对等能力 | 单 direct queue、整资源依赖、committed resource 复用。 | 没有异步 compute、跨队列同步、子资源跟踪、placed heap aliasing、并行 Pass 录制或缓存预算淘汰。它是实际可工作的基础 RDG，不是完整 UE RDG 复制。 |
| UE 操作覆盖 | 已约定的核心单选透视工作流。 | 没有多选/框选、正交视口、顶点吸附、持久枢轴或完整 UE 快捷键系统。 |
| 材质与屏幕效果质量 | 完整已列明表面参数、有限步屏幕查询和解析环境。 | Disney 加性模型不保证任意参数的严格能量守恒；屏幕 SSS 不是体积 BSSRDF，天空不是物理大气，SSR/SSIL 无法恢复不可见场景信息。 |
| 大规模与长时间验收 | CPU 契约、真实 GPU 回归及实际模型画面。 | 尚无固定射线逐项 DXR/BVH 对照、全部材质高样本 HDR 误差、长期动态稳定性、显存峰值及生产规模性能验收。 |

其中 SAH、Loose Octree、Hi-Z、NRD、TAA、异质密度和直方图曝光出现在早期 V1 设计中；这些历史规格仍是设计记录，不是已完成证明。Lumen、Nanite 按用户明确允许延期，当前没有实现，不计作本轮首版必须补齐的内容。

## 本轮更新

- Profile 已改为显式录制会话，空闲不采集、停止后逐帧回看、600 帧自动停止；说明和覆盖范围见 [性能录制](Profiling.md)。
- 常用物体栏提供七种配置化预设，接入共享 Resource、稳定对象身份、现有默认值与作者历史；拖放缓存避免静态预览逐帧复制模型纹理。CPU 契约与真实 ImGui 放置测试覆盖创建、取消、撤销、恢复、窄窗口及保存重开。
- 完整回归还定位并修复了空闲 fence 提前退休未提交上传存储，以及导入点光等零面积尺寸无法提升为世界文档的问题。验证记录区分首次失败和修复后的执行。

本工程已形成可运行的实验引擎和编辑工作流，但完整原始引擎范围仍有上述差额，不能以部分测试通过宣称所有需求全部完成。
