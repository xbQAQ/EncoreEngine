# V1：具备全局光照、阴影与后处理的 DX12 引擎

日期：2026-10-15。状态：修订设计，待用户确认。本文是本地设计过程产物，不加入 Perforce changelist。

## 用户确认的范围修订

第一版可以延后 Lumen 和 Nanite，但必须包含全局光照、阴影与后处理。首版保留原始需求中其他系统的基础实现；此前只包含简单直接光的 M1 改为内部联调节点，不作为正式第一版交付。

V1 的可见结果：启动 ImGui 编辑器即可查看并编辑 Cornell Box，切换混合光栅 / 光追模式与渐进路径追踪模式；看到多次反射产生的间接照明、红绿墙色溢、物体遮挡和软阴影，修改光源后照明更新；曝光、Bloom、抗锯齿与色调映射可交互调整。

## 架构与运行目标

沿用总体设计中的 Core、Platform、RHI.D3D12、RDG、Assets、Spatial、RenderData、Renderer、World、Runtime 和 Editor。每个模块以独立构建目标约束依赖。所有 GPU 上传、加速结构构建、绘制、追踪、降噪、后处理、编辑器合成和读回由 RDG 调度，交换链 Present 位于图外。

以本机 Windows x64 / RTX 5070 作为首个验证环境；实际 DXR 和 Shader Model 支持由 D3D12 运行时查询确认。Raster 基础模式保留不使用 DXR 的直接光和 shadow map 能力。正式 V1 的 GI / 路径追踪验收需要支持 DXR 的设备，缺少 DXR 时必须明确显示功能限制。

首版优先正确性、可交互性和可观察性，帧时间与噪声收敛均以实际测量报告，不预先承诺所有光追功能组合达到 60 FPS。

## 首版系统清单

| 系统 | V1 内容 | 验收要点 |
| --- | --- | --- |
| RDG | 资源与 Pass 声明、版本与依赖、拓扑排序、Pass 剔除、Barrier、生命周期、外部历史资源、执行诊断 | 全部 GPU 路径可追溯到图；图测试与 GPU 验证通过 |
| 空间结构 | SAH BVH、Loose Octree、DXR BLAS / TLAS | CPU 查询对照暴力结果，实例变换与结构更新正确 |
| 材质 | Material / MaterialInstance、Principled BRDF、GGX、镜面、BTDF、玻璃、发光与纹理 | CPU / HLSL 语义一致，采样和 PDF 对应，反射折射与全内反射可观察 |
| 全局光照 | 混合模式的 DXR 蒙特卡洛间接光、时空降噪，以及独立渐进路径追踪参考 | 红绿墙色溢、阴影区间接照明、离屏几何贡献、动态更新 |
| 光源 | 点光、聚光、方向光、矩形 / 圆盘面光及发光网格 | 单位、采样 PDF、距离衰减和光源选择概率有定义 |
| 阴影 | PCF、PCSS、Variance Shadow Maps 与面光源的采样可见性 | 可切换并观察结果；遮挡与半影变化正确，报告过滤近似与漏光限制 |
| 体渲染 | 均匀和 3D 纹理密度介质、吸收、单次散射、各向异性与体积阴影 | 零密度恒等、透射数值对照、Cornell 光柱与边界正确 |
| 剔除 | 距离、视锥、八叉树层级、背面、基础 GPU Hi-Z | 独立开关与统计，和禁用剔除的结果对照 |
| 后处理 | HDR、手动 / 自动曝光、Bloom、TAA、ACES 拟合色调映射、Gamma | 效果可开关，参数有效，运动与尺寸变化不会复用错误历史 |
| 编辑器 | ImGui 场景层级、属性、视口、材质 / 光源参数、管线和阴影选择、RDG、性能与日志 | 修改真实场景和渲染参数，输入捕获与资源生命期正确 |
| 资产与场景 | Blender 导出的 glTF 2.0 / GLB 静态模型、纹理与基础材质；版本化场景保存 / 加载 | 导入并显示资产，处理变换 / 坐标 / 纹理颜色空间，不支持扩展有明确诊断 |
| 示例 | Cornell Box 与剔除压力场景 | 能从编辑器启动、修改、保存和重开，截图可用于对照 |

## GI 方案与取舍

主方案采用与现有目标一致的 DXR 蒙特卡洛 RTGI：Raster 产生 GBuffer，从首个可见表面生成间接路径，使用与路径追踪器共享的 BSDF、光源采样和遮挡查询。反射深度与采样数可调；相机主射线命中的表面计为第 0 次，交互预览默认再追踪最多 2 次表面反射，UI 与积分器使用同一计数约定。

另一条可行路线是以 Probe / DDGI 缓存间接照明。它能摊销追踪成本，但增加探针布置、可见性插值与漏光调试。V1 选择直接 RTGI，先建立统一积分器和可验证的画质参考；Lumen 阶段再独立增加 Probe / Surface Cache 等机制。

RTGI 将直接光和间接光分开输出，不在 GI 信号中重复计算首个表面的直接照明。不透明表面的漫反射和粗糙镜面信号分别处理；采用 NRD REBLUR，并由原生 API 返回的 Compute 调度逐项生成 RDG Pass。深度、法线 / 粗糙度、运动向量和 hit distance 采用与 NRD 一致的坐标、编码与单位。

降噪前后信号均可查看。NRD 的永久历史资源由 Renderer 持有并逐帧导入，瞬态纹理由 RDG 管理。相机突变、渲染尺寸变化、场景重载和无效运动历史触发重置；材质 / 光照更改通过场景版本与重置策略防止明显残影。玻璃和体积不直接送入仅为不透明表面准备的 NRD 信号路径。

Path Tracing 模式从相机开始完整积分，使用 next-event estimation、MIS、重要性采样和 Russian roulette，支持离散镜面分支与 BTDF；每帧累计可配置的样本数，静止时逐渐收敛。材质、光照、相机、介质和积分参数变化后重置累计。保存原始 HDR 输出作为 GI 与阴影对照，不能只对比带 Bloom 或自适应曝光的 LDR 截图。

## 阴影与光源约定

- PCF：在 shadow map 上比较并过滤可见性；过滤范围、分辨率与 bias 在编辑器可调。
- PCSS：先搜索遮挡者，再按光源尺寸与遮挡几何估计半影；该选项用于 shadow map 的软阴影近似，明确其支持的投影类型。
- VSM：表示 Variance Shadow Maps，生成并过滤一阶 / 二阶深度矩，提供最小方差和 light-bleeding reduction；不与 UE 的 Virtual Shadow Maps 混称。
- 聚光 / 方向光先使用二维 shadow map，点光使用 cube shadow map，并针对 cube face 边界和过滤实现独立验证。未适配某种过滤 / 投影组合时禁用该组合并说明原因，不能静默切换成其他算法。
- 矩形、圆盘及发光三角形的真实软阴影通过有限面积采样与 DXR 阴影射线获得。主视图不可见的遮挡物仍保留在阴影集合和 TLAS 中。
- 漫反射和粗糙镜面混合表面按材质求光；镜面、玻璃和非 delta 光源使用与积分器一致的概率语义，避免错误 MIS 和重复计算发光项。

## 后处理与历史管理

1. 线性 HDR：直接光、GI、反射 / 透射与介质合成为场景线性颜色；保留后处理前读回入口。
2. TAA：使用相机 jitter、深度、运动向量、邻域限制与 disocclusion 检查；历史资源按渲染视口尺寸管理，透明和体积采用保守历史权重。
3. 曝光：提供手动 EV 与基于亮度直方图的自动曝光，适应速度与 delta time 相关。测试模式固定曝光并关闭时域抖动等非确定因素。
4. Bloom：在曝光后的 HDR 域提取亮部、降采样 / 上采样并合成，提供阈值、软膝和强度。首版将各层表示为独立纹理，兼容整资源状态跟踪。
5. Tone Mapping：采用 ACES 拟合曲线输出线性 LDR，通过 sRGB RTV 完成最终显示编码。ImGui 顶点颜色先转换为线性值再混合，字体 alpha 保持线性；采样彩色 UI 纹理使用对应的 sRGB SRV，避免重复应用 Gamma。V1 首先交付 SDR 显示输出。

抗锯齿历史、NRD 历史、路径累计与自动曝光状态各自管理，渲染模式切换不会相互复用不兼容历史。GI 和阴影对照时固定曝光、关闭 Bloom，避免后处理掩盖光照差异。

## 渲染图主流程

初始化 / 资源变化：UploadAssets -> BuildBLAS -> BuildTLAS；ImGui 字体和纹理上传同样注册到图。

混合模式：UploadFrameData -> UpdateTLAS -> Depth / GBuffer -> ShadowDepth 或 ShadowMoments -> ShadowFilter -> DirectLighting -> RayTracedIndirectAndReflections -> NRD 各 Compute Pass -> LightingComposite -> Transmission / Volumetrics -> TAA -> ExposureHistogram / Adaptation -> Bloom -> ToneMap -> ImGuiComposite -> 可选 Readback -> PRESENT。

路径模式：UploadFrameData -> UpdateTLAS -> PathTrace -> Accumulate -> Exposure -> Bloom -> ToneMap -> ImGuiComposite -> 可选 Readback -> PRESENT。离线参考导出从累计 HDR 分支读取；降噪若启用则作为独立显示分支，保留原始参考。

以上为数据依赖顺序，实际无依赖 Pass 的顺序可由图编译器选择。关闭某项功能时使用显式有效的恒等资源或删除相应分支，不允许读取未初始化纹理。NRD 多分辨率调度如需要逐 mip 读写，必须先完成 RDG 子资源跟踪或拆分纹理，不能绕过状态契约。

## 内部实现顺序

1. 基础设施与 RDG：构建模块、窗口与输入、设备与帧同步、图编译及 GPU 执行、内部 Cornell 直接光节点。
2. 场景与资产：World / RenderScene、材质与纹理、glTF / GLB、BVH / 八叉树和基本剔除。
3. 直接光与阴影：GBuffer、PBR、光源、PCF / PCSS / VSM 与调试视图。
4. DXR 积分：BLAS / TLAS、Principled BSDF、蒙特卡洛采样、镜面 / BTDF、面光阴影、渐进路径追踪及采样测试。
5. 混合 GI：接入共享积分器、运动数据、NRD 与历史失效，验证多次反射与动态更新。
6. 体积与后处理：介质、体积阴影、TAA、曝光、Bloom、ToneMap、Gamma。
7. 编辑器整合、基础 Hi-Z、场景读写、GPU / 图像回归与使用说明。

各节点可单独构建和验证，但只有全部通过首版验收后才宣布 V1 完成。Lumen / Nanite 对应的缓存 GI 与虚拟几何另外形成规格，不将普通 RTGI 或 Hi-Z 绘制改名冒充。

## 首版强制验收

- GI：固定 Cornell 场景与手动曝光，关闭直接光显示分支时仍有来自光源经其他表面反射的间接照明；红绿墙向白色表面产生对应色溢。GI 开关、反射深度和材质反照率变化影响原始 HDR；无环境常量替代项。
- GI 动态：移动光源、遮挡物或相机后照明更新；离屏遮挡物仍贡献阴影 / 间接光；新显露区域不永久残留旧历史。对照多样本路径参考报告误差和噪声。
- 阴影：PCF / PCSS / VSM 切换实际执行不同 Pass；改变接收面距离或面光源尺寸能观察半影变化；报告 VSM 漏光等近似限制。
- 后处理：分别关闭曝光适应、Bloom、TAA 和色调映射，读回前后输出证明效果存在。测试黑场、过曝亮部、高对比边缘、相机移动、resize 和模式切换。
- 材质 / 体积：镜面反射、玻璃折射 / 全内反射、粗糙度变化、零密度恒等、Beer-Lambert 透射具有可复现的对照场景。
- 资产 / 场景：导入 Blender GLB 静态资产；保存场景后重开恢复变换、光源和材质引用。错误资产不破坏当前已加载场景。
- 架构 / 稳定性：RDG、空间结构与采样测试通过；硬件 GPU 验证无未处理的错误；窗口缩放和历史资源重建后正常渲染，报告真实 adapter 和运行配置。

当前仅完成规格修订，以上均为拟实施与拟验证的要求，没有声称任何渲染功能已经实现或测试通过。

## 技术参考

- 总体层次与原始需求映射：`2026-10-15-dx12-engine-overview-design.md`。
- DXR 官方示例：https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-raytracing-samples-win32/
- NRD 输入、调度与原生 API 集成：https://github.com/NVIDIA-RTX/NRD
- 路径积分、MIS 与 Russian roulette：https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer
