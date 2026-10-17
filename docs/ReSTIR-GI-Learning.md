# ReSTIR GI 学习与实现说明

本文解释本项目中的 ReSTIR GI（Reservoir-based Spatio-Temporal Importance Resampling for Global Illumination）实现。目标是让读者能从路径空间的估计量推导到 GPU Pass、reservoir 布局、历史验证和调试指标，并能把结果与 Path Tracing、SSGI 进行对照。本文描述的是当前工程的有限历史、有限候选数版本；它是实用的有偏估计器，不宣称等同于无限样本或无偏的离线积分器。

## 1. 适用范围与代码契约

三种 GI 模式共享场景、材质和光源参数；混合式 SSGI/ReSTIR 消费光栅 GBuffer，Path Tracing 从相机独立积分：

| 模式 | 输入 | 可见性 | 典型用途 |
| --- | --- | --- | --- |
| Path Tracing | 相机路径与材质 | DXR 1.1 inline RayQuery 或 Compute 软件 BVH | 收敛参考、镜面/多次反射 |
| SSGI | 屏幕空间位置、法线、深度和历史 | 屏幕空间步进 | 低成本、屏幕内近似 |
| ReSTIR GI | Disney BSDF mixture 二次命中及其 radiance | DXR 1.1 inline RayQuery 或 Compute 软件 BVH | 漫反射/粗糙反射复用，尖锐反射独立追踪 |

公共 GPU 结构在 [`Source/Runtime/RenderCore/Public/render/GpuTypes.h`](../Source/Runtime/RenderCore/Public/render/GpuTypes.h) 中定义，根签名和资源槽位在 [`docs/superpowers/specs/2026-10-15-gi-interface.md`](superpowers/specs/2026-10-15-gi-interface.md) 中定义。相机使用右手坐标、Y-up、世界单位米、D3D 深度 0..1。当前 ReSTIR 资源位于 t11-t15/u3 等槽位，reservoir 采用 96 字节，即 6 个 `float4` lane。

[`shaders/Private/ReSTIRGI/ReSTIR.hlsl`](../shaders/Private/ReSTIRGI/ReSTIR.hlsl) 中的入口名称为：

- `CandidateMain`：每个当前 GBuffer 像素追踪二次表面并建立初始 reservoir。
- `TemporalMain`：重投影上一帧 reservoir，经过几何验证后合并。
- `SpatialMain`：在当前帧邻域内进行有限次数的空间复用。
- `EvaluateMain`：在当前 primary surface 处重算 RGB 面积积分核和可见性，输出 GI；粗糙度低于 0.35、含 clearcoat 或透射的 primary 使用完整相机路径回退。

公共投影和 GPU 结构入口位于 [`shaders/Private/Common/Common.hlsli`](../shaders/Private/Common/Common.hlsli)。求交、纹理求值、Disney BSDF、多光源采样和路径积分分别拆分到 `RayTracing/SceneQueries.hlsli`、`Materials/MaterialSampling.hlsli`、`Materials/Disney.hlsli`、`Lighting/LightSampling.hlsli` 和 `PathTracing/PathIntegration.hlsli`。下文区分代码当前实现和后续建议；实际验证范围见 [`Validation.md`](Validation.md)。

## 2. 为什么需要 ReSTIR GI

直接路径追踪对每个像素独立采样。间接光的高贡献路径通常很稀疏：一次随机 BSDF 方向没有击中重要的二次表面时，样本贡献接近零；而增加每像素样本数又会线性增加求交和着色成本。ReSTIR GI 把每个二次表面视为“会向 primary surface 发光的样本”，在相邻像素和相邻帧之间重用这些样本。这样，贵的二次路径只需少量产生，筛选和重用便可把它们传播给可能受益的接收点。

这与 ReSTIR DI 的对象不同：DI 重用的是直接影响 primary surface 的灯光样本；GI 重用的是从 primary surface 出发、经方向采样射线命中的二次表面或后续路径。[NVIDIA RTXDI 集成说明](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md) 要求二次位置与朝向、找到它的固角 PDF、以及二次表面或从它开始的路径 radiance。原始算法背景见 [Ouyang 等人的论文页面](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing)。

### 2.1 与 Path Tracing、SSGI 的边界

- Path Tracing 保留完整路径积分，适合做慢速参考，但每个像素都要承担新的路径成本。
- SSGI 只能使用当前屏幕和历史屏幕中的信息，屏幕外物体、遮挡后物体和完整多次反射会缺失，必须把这些限制显示给使用者。
- ReSTIR GI 可以重用屏幕上存储的二次结果，但仍然受历史有效性、二次 radiance 近似和有限候选数影响。当前实现的 secondary Lo 主要是漫反射近似，不是穷尽的 Disney GI，也不保证尖锐焦散。

## 3. 从路径积分到二次表面样本

设 primary surface 为 `x`，法线为 `n_x`，视线方向为 `v`；一次方向采样射线命中 secondary surface `y`，其法线为 `n_y`。记从 `x` 指向 `y` 的单位向量为 `omega_xy`，距离为 `r = |y-x|`。

一次间接光估计可写为：

```text
L_indirect(x, v) = integral over y of
    f_x(v, omega_xy) * L_o(y, -omega_xy) * V(x,y)
    * cos_x * cos_y / r^2 dA_y
```

这里 `f_x` 是 primary 的 BRDF，`L_o(y, -omega_xy)` 是 secondary 向 primary 发出的 radiance，`V` 是可见性，`cos_x = max(0, dot(n_x, omega_xy))`，`cos_y = max(0, dot(n_y, -omega_xy))`。`n_x` 是 BRDF 使用的 shading normal；`n_y` 必须是面积测度对应的 geometric normal，不能用法线贴图后的 shading normal 替代。代码用 `AreaContribution` 计算不含 `V` 的 RGB 面积积分核，用 `Target` 取其非负亮度。对 secondary surface 使用面积测度 `dA_y` 时，若初始射线的采样 PDF 以 solid angle 表示，则应使用面积与固角的 Jacobian：

```text
p_A(y) = p_omega(omega_xy) * cos_y / r^2
```

本项目将候选的 area measure 写成：

```text
qA = qSolidAngle * cosSecondary / r^2
```

候选的正标量目标（`target`）使用积分核的亮度：

```text
target(x,y) = luminance(
    f_primary(x, v, omega_xy)
    * outgoingRadiance(y)
    * cosPrimary * cosSecondary / r^2
)
```

`target` 是用于 weighted reservoir sampling 的未归一化目标值，不是候选采样 PDF。候选权重为 `w = target / qA`，只在 `qA > 1e-12` 时计算；非有限或负权重在 `ReservoirUpdate` 中按零处理。最终 Evaluate Pass 重新用当前接收点计算 RGB 面积积分核并查询 visibility。

### 3.1 固角 Jacobian 只应用一次

同一套公式必须使用相同测度：`qA` 由固角 PDF 乘 `cosSecondary / r^2` 得到，面积积分核也包含同一几何因子，所以在初始候选 `target/qA` 中会正确消去它。不能只转换 PDF 而保留固角积分核，也不能只转换积分核而保留固角 PDF。

把同一个 secondary 世界点从源接收点 `x_s` 重连到当前接收点 `x` 时，代码还计算固角映射比例：

```text
J_omega = (cosSecondary_at_x / |y-x|^2)
          / (cosSecondary_at_xs / |y-xs|^2)
```

`ReconnectionJacobian` 在 `J_omega` 不有限、低于 0.05 或高于 20 时拒绝复用。这是控制极端重连的有偏稳定性策略。样本始终保存在同一表面的面积域，恒等映射的面积 Jacobian 为 1，`MergeReservoir` 因而不再把 `J_omega` 乘到权重上。若在 `targetHere * W_source * M_source` 之后再乘一次固角 Jacobian，就会重复计算几何变换。

## 4. Weighted Reservoir Sampling 推导

### 4.1 单个候选

对候选集合 `S = {s_i}`，候选分布为 `q_i`，目标函数为 `pHat_i`，定义：

```text
w_i = pHat_i / q_i
wSum = sum_i w_i
M = number of represented candidates
```

流式更新时以概率 `w_i / wSum` 选择当前代表样本。所选样本 `s` 的归一化估计权重为：

```text
W = wSum / (M * pHat_s)
```

`pHat` 是标量，最终 RGB 估计使用 `F_RGB(s) * W`，其中 `F_RGB` 是 `AreaContribution` 的结果；不能把标量 target 直接当 RGB 颜色，也不能把 `outgoingRadiance` 乘两次。给定独立的初始候选集，按 `w_i/wSum` 选择样本后，有：

```text
E_selection[F_RGB(s) * W]
    = (1/M) * sum_i F_RGB(s_i) / q_i
```

这说明 reservoir 初始选择保留了普通重要性采样估计的条件期望。该结论不自动证明经过相关历史、邻域筛选和近似 secondary Lo 后的整个系统无偏。若 `M=0`、target 非正、`W` 非有限或没有有效代表，Evaluate 输出零。

### 4.2 合并另一个 reservoir

源 reservoir `R_s` 代表 `M_s` 个候选，保存 `wSum_s`、源选中样本的 `target_s` 和 `W_s`。在接收点 `x` 处，重新评估同一 selected sample 的目标得到 `target_here`。合并质量的核心表达为：

```text
M_source_used = min(M_source, maxHistoryM)
mergeWeight = target_here * W_source * M_source_used
```

这等价于把源流质量转换到当前接收点，再把该流作为 `M_source_used` 个候选参与 weighted reservoir sampling。初始候选中，零权重候选也增加 `M`，否则会错误增亮结果。GPU `MergeReservoir` 对无效源 reservoir 直接拒绝；CPU 通用参考 [`Source/Runtime/RenderCore/Public/render/Reservoir.h`](../Source/Runtime/RenderCore/Public/render/Reservoir.h) 的 `Merge` 还允许空源流只累加成员数。两者在全空源的复用策略上不同，CPU 参考用于检查权重语义，不是逐行复刻 GPU 策略。

### 4.3 有限历史和相关复用

时域和空间复用会让 `M` 很快增长。`FinalizeReservoir` 把 `M` 限制到 `FrameConstants.restir.x` 指定的最大历史成员数，并按 `maxM / M` 缩放 `wSum`，保持当前 `W`。缩放本身不会在这一刻改变估计值，但会改变这份历史以后参与合并的相对权重。相关复用、样本支持集变化、极端重连拒绝与近似 secondary Lo，使整个实用版本不能宣称无偏；`M` 也不能解读为独立样本数或实际历史帧数。

### 4.4 单候选数值检查

设 `x=(0,0,0)`、`y=(0,1,0)`，两者法线相向，因此 `cosPrimary=cosSecondary=1`、`r=1`。primary 是反照率 0.5 的 Lambertian，secondary 的三个 RGB 通道均为 `Lo=2`。cosine 方向提案给出 `qSolidAngle=1/PI`，于是 `qA=1/PI`，RGB 积分核与亮度 target 都为 `1/PI`。

单个候选有 `w=1`、`M=1`、`W=PI`，可见时最终值为 `(1/PI)*PI=1`，与普通 cosine 路径估计 `albedo*Lo=1` 一致。这是检查候选和 Evaluate 是否重复除 PDF、重复乘 radiance 或几何项的最小例子；它不是多候选或时空复用无偏性的证明。

## 5. Reservoir GPU 布局

每像素 96 字节，六个 `float4` lane。字段语义如下：

| lane | shader 字段 | xyz | w |
| --- | --- | --- | --- |
| 0 | `secondaryPositionPdf` | secondary world position | 原始 `sourceAreaPdf` |
| 1 | `secondaryNormalTarget` | secondary geometric normal | 当前接收点的 `selectedTarget` |
| 2 | `radianceWeightSum` | direction-independent `outgoingRadiance` | `weightSum` |
| 3 | `receiverPositionM` | 该 reservoir 完成归一化时的 receiver position | `M` |
| 4 | `receiverNormalW` | 该 reservoir 的 receiver normal | `W` |
| 5 | `directionDistance` | 当前 receiver 到 secondary 的方向 | 距离 |

当前六个 lane 均为 `float4`，`M` 以浮点数存储并受有限历史上限约束。`ReservoirValid` 要求 `M`、`W` 和 selected target 为正，并检查 radiance/weightSum、位置/PDF 的有限性。GPU stride 仍是 96 字节；变更布局时必须同时修改 C++ 资源创建和 HLSL 声明，不能仅调整文档。

`FinalizeReservoir` 在每轮复用结束时更新 receiver position/normal 和 `W`，所以它们指向源 reservoir 上一轮实际归一化的接收点，并非永远不变的最初相机命中点。`directionDistance` 随之更新，主要用于诊断；最终重连会从当前 receiver 和 secondary 位置重新计算射线。

## 6. RDG Pass 与每帧数据流

初始化图上传三角形、材质和 CPU 构建的软件 BVH；DXR 可用时另有 `DXR.BuildBLAS`、`DXR.BuildTLAS`。ECS `World.ExtractRenderSnapshot()` 只复制可见网格、活动相机和启用光源。快照 revision 改变时，Renderer 重新提取场景并比较几何/材质字节，上传变化的数据；几何变化还会重建软件 BVH，并在 DXR 可用时重建 BLAS/TLAS。没有 revision 变化时不重复构建 AS。逐帧 ReSTIR 分支由 `Source/Runtime/Renderer/Private/ReSTIRGI/ReSTIRGIPass.cpp` 组合：

```text
Shadow.RasterDepth [PCF/PCSS/VSM，可选]
  -> Shadow.BlurHorizontal -> Shadow.BlurVertical [仅 VSM]
  -> Raster.GBuffer -> Lighting.Direct
  -> ReSTIR.Candidates [CandidateMain]
  -> ReSTIR.Temporal [TemporalMain]
  -> ReSTIR.Spatial [SpatialMain]
  -> ReSTIR.Evaluate [EvaluateMain]
  -> Resolve.Temporal [TemporalMain]
  -> Resolve.Spatial [SpatialMain]
  -> Post.VolumeBloomACES
  -> Editor.ImGui

ReSTIR.Spatial -> History.Reservoir
Lighting.GI   -> History.GI
GBuffer       -> History.Position + History.Normal
Resolve.Temporal -> History.Resolved
```

这是数据依赖示意；无直接依赖的 Pass 可由图编译器选择执行顺序。[`RadianceResolve.hlsl`](../shaders/Private/Denoiser/RadianceResolve.hlsl) 对 ReSTIR 静止画面累计线性 HDR 均值，历史有效性仍检查位置、法线与材质；该均值不做短窗口 sigma clipping，也不随 reservoir M 的上限停止积累。World/camera revision 或参数 reset 立即清空均值。SSGI 仍采用兼容 3x3 邻域的 1.75 sigma clipping 与短历史重建。随后的小型 cross-bilateral 滤波只处理混合式近似像素，完整 BSDF 路径像素保持像素积分结果。`History.Resolved` 保存 temporal 输出，spatial 输出只交给后处理。这不是 NRD 或完整 TAA；只有 PT 与完整路径回退进行像素内随机采样，光栅 GBuffer 无 jitter，尚无动态对象 motion vectors。

交换链 `Present` 在 RDG 执行之后由平台调用，backbuffer 导出到 `PRESENT` 状态。GPU 上传、BLAS/TLAS build、draw/dispatch 和读回有显式图访问声明；frame constant 的 CPU 映射写入不属于 GPU 命令。初始、时域、空间输出使用独立资源，空间 Pass 从不可变的 temporal reservoir 读取，避免原地邻域反馈竞争。Evaluate 将 spatial buffer 绑定到逻辑 `CurrentReservoir`（t11），不要只按槽位名称误判它在读 initial candidates。

### 6.1 CandidateMain

1. 从当前 position/normal GBuffer 与材质表载入 receiver；由像素坐标和 frame index 生成逐帧变化的随机序列。
2. 用 `SampleBsdf` 采样完整 Disney proposal mixture（含 GGX VNDF），通过 `TraceScene` 求最近交，`qSolidAngle` 使用返回的完整 mixture PDF。每像素每帧新候选数为 `clamp(Counts.w,1,32) * clamp(SPP,1,16)`；UI 的 Candidates 提供每个 sample 的 1..8 个提案，SPP=16、Candidates=2 实际生成 32 个提案。`Settings.w` 的深度包括 primary vertex；小于等于 1 时或该像素使用完整路径回退时 Candidate 输出空 reservoir。
3. 对 secondary surface 调用 `DiffuseOutgoing`，沿漫反射后续路径估计方向无关的 `outgoingRadiance`。首次命中只扣除已由解析面光 NEE 覆盖的 emission，保留普通发光网格的 emission 和所有散射项；miss/零目标保留为零权重提案并计入 `M`。解析灯身份来自三角形的独立 tag，不能只按可复用的材质 ID 识别。
4. 计算 `qSolidAngle`、`qA`、target 和 `w=target/qA`，把候选流更新到 t11/u3 的 current reservoir。
5. 经 `FinalizeReservoir` 写入当前 receiver、selected direction 和归一化 `W`，便于后续历史验证、重连和诊断。

法线贴图只改变 shading normal，不改变实际三角形的几何边界。初始 BSDF 提案落到 receiver geometric normal 背侧或产生 null event 时仍计入提案数，不重新抽样；`AreaContribution` 同样拒绝跨几何半球的重连，因此 target、temporal/spatial merge 和 Evaluate 遵守同一支持集。visibility 的射线偏移使用 geometric normal，不能用它替代提案/重连的半球检查。

尖锐反射的 primary lobe 对邻域重连高度敏感，当前 secondary Lo 又不保留方向依赖，因此不把它作为可靠的 glossy ReSTIR。`UsesPathFallback` 对 base roughness < 0.35、任意非零 GTR1 clearcoat、specular transmission 或 diffuse transmission 使用完整 `TracePath`，以 SPP 次随机像素内射线积分，包含正确 Disney 散射和直接光。合成时不再叠加 `DirectBuffer`，避免重复计光；这些像素也跳过漫反射空间滤波。该工程回退不等于实现了任意 glossy/BTDF 路径的 ReSTIR 重连。

UI 的 Accumulated SPP 是静止画面的累计采样预算，Fresh proposals/frame 是本帧 RIS 新提案预算；两者都不等于 reservoir `M`，相关复用也不能当作独立新样本。

### 6.2 TemporalMain

当前实现把 receiver world position 投影到上一帧相机（`ProjectPosition(..., true, ...)`），读取 t9-t10 的历史几何和 t13 的 previous reservoir。它假定静态几何，没有动态对象 motion vector 或 deforming surface correspondence。只有同时满足以下条件时才合并：

- 重投影坐标在上一帧有效范围内，且相机空间深度在 near plane 之后。
- `CompatibleHistory` 检查历史有效标记、相同材质 ID，以及 normal dot 大于 `Restir.z`。
- 当前和历史 world position 差小于 `max(0.025, Restir.w * cameraDistance)`；这是静态场景的 normal/depth 近似验证。
- 源 reservoir 有效且其固角重连比例通过 `[0.05,20]` 稳定性范围检查。

失败时保留当前候选。`Flags.x` 为 reset 时禁止时域合并，`Flags.y` 为零时关闭时域复用；`FinalizeReservoir` 限制 `M`。当前 reservoir 没有独立 age 字段，不能把 `M` 直接解释成经过的帧数。宿主应在相机跳变、resize、GI/backend 切换和场景/材质/光源变化时设置 reset。

### 6.3 SpatialMain

当前实现使用随机旋转的黄金角分布，最多读取 6 个邻居，半径由 `Restir.y` 控制，`Flags.z` 控制开关。`CompatibleNeighbor` 比较有效标记、相同材质、normal dot、切平面法向距离、世界距离和粗糙度差（小于 0.2）。通过过滤的 reservoir 在当前 receiver 重新评估 target 后合并。空间半径越大，传播越快，但跨边缘污染和偏差风险越高；固定六邻居仍具有相关性，不等价于六个独立新路径。

### 6.4 EvaluateMain

对最终 selected secondary point 重新建立 `x -> y` 向量和距离，重算 primary BRDF、`cosPrimary`、`cosSecondary` 与 visibility。target 已在上一阶段 `FinalizeReservoir` 中对该 receiver 重算并用于 `W`。最终贡献为：

```text
L = visibility(x,y)
    * f_primary(x, v, omega_xy)
    * outgoingRadiance(y)
    * cosPrimary
    * cosSecondary / r^2
    * W
```

这里 `W` 已经包含 reservoir 的 `M` 归一化语义；不要在 Evaluate 再乘一次 `1/qA` 或再次应用固角 Jacobian。`SegmentVisible` 采用与 Candidate 相同的 `TraceScene` 后端，以有限 `tMax` 检查当前 primary 到 secondary 的遮挡；它目前是最近交查询，不是专门的 any-hit 优化。关闭 Shadows 时 `Flags.w=0` 会跳过该检查，此时不能作为物理正确性参考。

`UsesPathFallback` 的条件与 6.1 节一致：base roughness < 0.35、clearcoat > 0、specular transmission > 0 或 diffuse transmission > 0。每帧 1..16 条完整相机路径以像素内随机偏移进行积分，随后累计 HDR 均值，不重复叠加 primary emission/直接光，也不做漫反射邻域滤波。该策略保留尖锐反射、清漆和透射表现，但不代表 ReSTIR GI 支持任意镜面/透射 shift mapping。有限样本的完整路径仍有 Monte Carlo 噪声；验收应与同预算独立 PT 的噪声对照，而不是要求有限 SPP 完全无噪声。

## 7. DXR 与软件 BVH 后端

两种后端必须消费同一套三角形、材质和坐标约定，并返回同义的最近命中/可见性结果：

| 后端 | Candidate/最终 visibility | 典型问题 |
| --- | --- | --- |
| DXR 1.1 inline RayQuery | 当前 Compute shader 访问 t3 TLAS，通过 `RayQuery<RAY_FLAG_NONE>` 遍历非 opaque 候选，经统一材质裁剪后 commit | 设备能力、AS 更新、实例 mask、驱动差异 |
| Compute software BVH | 读取 t0 `GpuTriangle` 和 t1 `GpuBvhNode`，执行 AABB slab + 三角形求交；64 项栈异常/溢出时完整扫描 | 数值鲁棒性、极端树形退化、遍历开销 |

后端切换不应改变 reservoir 的 PDF 和 target 定义。两端都通过 winding 算几何法线，按单面/双面与透射规则接受表面，并使用同一 UV/alpha MASK 阈值过滤候选；玻璃允许从内外两侧求交。路径透射由 Disney BSDF 处理，阴影仍是二值可见性，没有 fractional/彩色透射阴影。光栅 GBuffer 和阴影 atlas 使用相同的表面裁剪规则。应先用固定射线集比较命中距离、三角形 ID、法线和 segment visibility，再比较 GI 图像。DXR 不可用时必须报告能力状态并显式使用 software backend，不能以空输出伪装成功。

## 8. 历史失效与版本管理

当前历史资源包含 previous GI、previous position、previous normal、previous reservoir 和 resolved HDR。宿主把 reset 传入 `Flags.x`；UI 对 GI/backend、候选数、bounce、复用开关、M/半径、阴影、介质和相机改动设置 reset。`World.MarkDirty()` 提升快照 revision，作为 ECS 场景、材质、光源和模拟变化的统一入口；Renderer 同步场景并重置不兼容的 GI 历史。扩展运行时热重载时，还应覆盖以下事件：

- 分辨率/交换链尺寸变化，窗口从最小化恢复。
- `--gi` 模式或 `--backend` 后端变化。
- 相机位置、朝向、FOV 发生大跳变；相机 cut 或投影参数改变。
- 三角形、材质、光源、BVH/TLAS 或介质版本变化。
- 采样定义、shader permutation、reservoir 布局发生变化。

小幅相机运动在未触发全局 reset 时可走静态位置相机重投影，并进行 normal/position history checks。发现 disocclusion、法线翻转或材质不匹配时丢弃对应像素历史。当前编辑器相机操作会保守地设置 reset；动态几何或光源修改也通过 World revision 使历史失效。若要在动态对象运动中保留历史，仍需对象 motion、per-object version 和前帧几何支持，不能把相机重投影当作完整 motion vector 系统。

## 9. 伪代码（与当前入口对齐）

以下为概念伪代码，变量名对应 `CandidateMain`、`TemporalMain`、`SpatialMain`、`EvaluateMain` 的职责；具体资源绑定以 shader 的实际声明为准。

```text
CandidateMain(pixel):
    x = LoadReceiver(pixel)
    r = EmptyReservoir()
    if !x.valid or pathDepth <= 1: StoreCurrent(r); return
    repeat candidateCount times:
        ray = CosineHemisphere(x.normal, rng)
        hit = VisibilityQuery(ray, backend)
        if !ValidSecondary(hit):
            r.M += 1
            continue
        y = SecondarySample(hit)
        y.outgoingRadiance = DiffuseOutgoing(hit)
        qSolidAngle = max(dot(x.normal, ray.direction), 0) / PI
        distanceSquared = dot(y.position - x.position, y.position - x.position)
        qA = qSolidAngle * max(dot(y.normal, -ray.direction), 0) / distanceSquared
        target = Luminance(PrimaryBRDF(x, ray.direction)
                           * y.outgoingRadiance
                           * cosPrimary * cosSecondary / distanceSquared)
        y.selectedTarget = target
        weight = qA > epsilon ? target / qA : 0
        ReservoirUpdate(r, y, weight, 1, rng)
    FinalizeReservoir(r, x)
    StoreCurrent(r)

TemporalMain(pixel):
    x = LoadReceiver(pixel)
    current = LoadCurrent(pixel)
    previousPixel = ProjectPosition(x.position, previousCamera)
    previous = LoadPrevious(previousPixel)
    if HistoryEnabledAndCompatible(x, previousPixel, previous):
        MergeReservoir(current, previous, x, rng)
    FinalizeReservoir(current, x)
    StoreTemporal(current)

SpatialMain(pixel):
    x = LoadReceiver(pixel)
    r = LoadTemporal(pixel)
    for neighbor in SixGoldenAngleNeighbors(pixel, spatialRadius):
        n = LoadTemporal(neighbor)
        if CompatibleNeighbor(x, neighbor):
            MergeReservoir(r, n, x, rng)
    FinalizeReservoir(r, x)
    StoreSpatial(r)

EvaluateMain(pixel):
    x = LoadReceiver(pixel)
    if IsPerfectMetalOrGlass(x):
        return TraceCameraPathFallback(x)
    r = LoadSpatial(pixel)
    if !r.valid: return 0
    connection = Reconnect(x.position, r.secondaryPosition)
    visible = VisibilityQuery(connection, backend)
    contributionRGB = AreaContribution(x, r)
    return visible * contributionRGB * r.W
```

伪代码中的 `target/qA`、`targetHere * W_source * M_source` 和 `W=wSum/(M*target)` 是必须保持一致的三个环节。完整合并还需要代码中的 `ReservoirValid` 和极端重连拒绝；每个阶段末调用 `FinalizeReservoir`。把 target 当 PDF，或在面积域合并之后再乘固角映射 Jacobian，都会造成系统性亮度错误。

## 10. 调试视图与指标

当前 `EvaluateMain` 已按 `FrameConstants.sampling.w` 提供 0=GI、1=`M` 热力图、2=`W` 对数热力图、3=法线。下列指标和视图是扩展验证建议，不能理解为均已实现：

- `weightSum` 和 selected target。
- 当前候选/历史/空间合并接受率，空 reservoir 比例，独立的历史年龄计数。
- secondary position、normal、distance、qA 和 target 的 false-color。
- visibility 命中/遮挡、DXR 与 software BVH 的差异 mask。
- temporal rejection 原因（越界、深度、法线、材质/版本、NaN）。
- GI raw、RadianceResolve 前后、Bloom/ACES 后的对比；禁止只看 tone-mapped 结果判断积分是否正确。

验证工具应记录：平均和 P95 `M`、有效 reservoir 百分比、候选与复用计数、target/wSum 非有限计数、历史 reset 原因、`RadianceResolve` 的 history rejection/clipping 计数、每个 RDG Pass GPU 时间、backend 和分辨率。当前 UI 展示 CPU 帧时间和实际 RDG Pass 列表，尚未提供全部统计或 GPU timestamp 读回。出现闪烁时先看 rejection 与 `M/W`，再检查 resolve 的位置/法线/材质兼容和 sigma clipping；出现整体过亮/过暗时先检查 qA/Jacobian 是否重复、源 reservoir 的 `M` 是否被正确保留，以及最终 visibility 是否只执行一次。

## 11. 验证计划

### 11.1 数学和 CPU 参考

- 对 `WeightedReservoir` 测试零权重、正权重选择概率、`M` 累加、`W` 有限性、合并质量和 `ClampHistory`。
- 使用固定种子与大量重复试验比较 reservoir 估计的均值/方差；不以单帧截图证明无偏。
- 检查 `qSolidAngle -> qA` 的单位/数量级，确保 `cosSecondary <= 0` 时拒绝候选。

### 11.2 图形和后端对照

- 固定 Cornell Box：Path Tracing 多样本结果作为慢速参考，比较 ReSTIR 的均值、方差、色溢和阴影边缘。
- 同一帧、同一随机种子分别运行 DXR 与 software BVH，先比较命中 ID/距离，再比较 raw GI。
- 移动相机、光源和遮挡物，检查 disocclusion 是否在有限帧内恢复；resize/minimize/restore 后检查历史 reset。
- 在低粗糙镜面、背面、屏幕边缘、屏幕外遮挡和发光面上单独采集图像与指标。

### 11.3 成功标准和不能声称的内容

验证应报告实际命令、设备、分辨率、帧数、Pass GPU 时间和失败项。当前设计不声称集成 Lumen、Nanite 或完整 RTXDI，也不声称 ReSTIR GI 对任意 Disney 材质和焦散无偏。SSGI 的屏幕空间缺失、secondary diffuse Lo 近似、有限历史与相关复用属于已知限制。

当前 PNG capture 来自后处理后的 `Post.Display`，不包含编辑器边框。它适合检查输出非空、构图和可见异常；定量 HDR 误差、候选数统计和逐像素后端命中对照需要额外读回工具，不能用 sRGB PNG 代替。

## 12. 复杂度与内存估算

令屏幕像素数为 `P`，每像素初始候选为 `K`，二次漫反射路径深度为 `B`，时域/空间合并总数为 `R`（当前最多 1+6），则工作约为 `O(P * (K*B + R + 1))` 次着色/评估。每个初始候选和 secondary bounce 要求交，普通复用阶段只重算目标与稳定性检查，Evaluate 再检查一次接收点到二次点的可见性；delta fallback 另付相机路径代价。软件 BVH 退化到完整扫描时单次求交为 `O(T)`，其中 `T` 是三角形数。性能仍需 GPU 测量，不能只按 reservoir 合并次数推定帧率。

单张 reservoir 内存为 `P * 96` 字节。当前 runtime 创建 Candidates、Temporal、Spatial、Previous 四张全分辨率 buffer：640x480 时单张 28.125 MiB，合计 112.5 MiB；1920x1080 时四张合计约 759.4 MiB。该数字尚未计入 GI/GBuffer、Resolve temporal/spatial/HDR history、BVH/TLAS 和调试输出。当前资源没有别名复用；优化可从打包布局、缩小工作分辨率和阶段缓冲复用开始，但必须重新验证读写 hazard 与 stride。

## 13. 常见错误排查

| 现象 | 优先检查 |
| --- | --- |
| 全屏过亮 | 面积域合并后是否又乘固角 Jacobian；Evaluate 是否又乘了一次 `1/qA`；`W` 是否缺少 `M` |
| 全屏过暗 | `qA=0` 候选未拒绝；selected target 为零；visibility 被执行两次或 radiance 未保留 |
| 细节拖影 | history reset/version；motion 深度符号；normal/depth threshold；`M`/年龄上限 |
| 物体边缘漏光 | 空间邻域过滤过宽；secondary reconnection visibility；相邻材质/实例未比较 |
| 墙脚/箱底接缝漏光 | GBuffer 接收点是否越过实际几何；起点偏移之外是否又裁掉近距离遮挡；零距离边界是否在两后端一致 |
| DXR 与软件差异大 | TLAS/BVH 三角形顺序、背面规则、epsilon、tMin/tMax、实例变换和 mask |
| 镜面噪声稳定但稀疏 | ReSTIR GI 对低粗糙 specular 的限制；与原始路径样本做 MIS 或回退 |
| 数值爆炸 | 检查 `qA`、target、wSum、W 的 finite/epsilon/clamp；拒绝 NaN/Inf |

本工程接触边界回归还检查背墙外、地板下和侧墙外光源的 raw HDR 零贡献。GBuffer 和 ray hit 共用有界重心位置重建，射线不再叠加第二层 `tMin` 近裁剪；边界覆盖样本保留覆盖并选择极近的三角形内部代表点。实现细节、实际负坐标探针和金属反射对照见 [`ContactVisibility.md`](ContactVisibility.md)。金属表面反射地板产生的白亮不应直接当成穿透漏光，也不能仅凭 tone-mapped 图像替代遮光控制实验。

## 14. 参考资料

1. Yaobin Ouyang 等，[*ReSTIR GI: Path Resampling for Real-Time Path Tracing*](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing)，NVIDIA Research，2021-06-24。
2. NVIDIA-RTX/RTXDI，[*Doc/RestirGI.md*](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md)：集成模型、输入、二次表面近似和最终着色。
3. NVIDIA-RTX/RTXDI，[*Doc/ShaderAPI-RestirGI.md*](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/ShaderAPI-RestirGI.md)：reservoir API、合并与归一化。注意 SDK 的 `weightSum` 在 finalize 后可表示归一化逆 PDF；本项目独立保留 `weightSum` 和 `W`，字段名不能直接等同。
4. 本工程已批准接口：[`docs/superpowers/specs/2026-10-15-gi-interface.md`](superpowers/specs/2026-10-15-gi-interface.md)。
