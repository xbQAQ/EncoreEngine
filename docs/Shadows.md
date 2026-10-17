# 阴影贴图实现说明

本模块使用实际光栅化 shadow atlas，为直接光提供 PCF、PCSS 和 VSM。它与 DXR/软件 BVH 的射线可见性并存。三种 GI 的直接光采样均遵循所选阴影模式，包括 Path Tracing：模式 0 使用射线作为可见性参考；PCF、PCSS 和 VSM 会引入近似可见性偏差，因此不能将这些模式下的 PT 宣称为无偏参考。

## 1. 代码与接入边界

| 入口 | 职责 |
| --- | --- |
| [`ShadowViews.h`](../Source/Runtime/Renderer/Public/render/ShadowViews.h) / [`ShadowViews.cpp`](../Source/Runtime/Renderer/Private/Shadows/ShadowViews.cpp) | 验证光源/容量，建立 CPU 阴影视图并分配 atlas tile |
| [`ShadowRenderer.cpp`](../Source/Runtime/Renderer/Private/Shadows/ShadowRenderer.cpp) | 创建资源，注册光栅、水平模糊和垂直模糊 RDG Pass |
| [`RasterShadowDepth.hlsl`](../shaders/Private/Shadows/RasterShadowDepth.hlsl) | 对共享三角形做 VS/PS 光栅，写入线性深度的一二阶矩 |
| [`VSMGaussianBlur.hlsl`](../shaders/Private/Shadows/VSMGaussianBlur.hlsl) | 不跨 tile 的九 tap 分离卷积 |
| [`ShadowSampling.hlsli`](../shaders/Private/Shadows/ShadowSampling.hlsli) | 接收点投影、PCF、PCSS 和 VSM 可见性 |
| [`ShadowTests.cpp`](../tests/ShadowTests.cpp) | 视图覆盖、裁剪、分配容量、失败原子性与过滤数学 |

`ShadowRenderer` 在图导入前创建资源，资源的实际生命周期由 RHI `ResourcePool` 持有。调用 `AddPasses(graph, context, viewCount, shadowMode)` 返回本帧采样用的 atlas：PCF/PCSS 为原始矩图，VSM 为垂直模糊输出。`Atlas(mode)` 可在图注册前用于固定描述符设置。模式 0 返回原始资源但不注册阴影贴图 Pass，由光照层使用射线可见性。

## 2. GPU 契约

`GpuShadowView` 为 80 字节，与 HLSL `ShadowView` 的五个 16 字节 lane 对齐：

| 字段 | xyz / xy | w / zw |
| --- | --- | --- |
| `originNear` | 光源视图原点 | 正 near 距离 |
| `rightFar` | 单位 right | far 距离 |
| `upExtent` | 单位 up | 透视 `tan(halfFov)` 或正交 half extent |
| `forwardProjection` | 单位 forward | 1 为透视，0 为正交 |
| `atlas` | tile 原点 x/y，像素单位 | tile 分辨率、所属 light index |

`GpuLight.shadow.xy` 保存连续视图的起始索引与数量；`shadow.zw` 保持原有 emitter/material 元数据。最多 16 个光源、96 个视图，16 个点光恰好需要 96 个视图。`BuildShadowViews` 超限、收到非法投影或非法场景包围盒时显式失败；全部视图构建成功后才提交光源范围，避免部分光源被提前修改。

资源约定为 t16 `Lights`、t17 `ShadowAtlas`、t21 `ShadowViews`。b1 在根参数 3 中提供四个 32 位常量，第一项为 `ShadowViewIndex`。`FrameConstants.lighting` 包含光源数、视图数、模式和 atlas 列数；`shadow` 包含 tile 分辨率、深度偏移米数、法线偏移米数和 PCF 半径；`variance` 包含最小方差与漏光抑制。

## 3. 阴影视图

| 光源 | 视图 | 投影约定 |
| --- | --- | --- |
| Directional | 1 | 沿 emission direction 的正交投影，保守拟合场景 AABB 八角点并加入边界余量 |
| Point | 6 | 世界 ±X/±Y/±Z 方向，90° 正方形透视，每个面独立 tile |
| Rectangle / Disk | 1 | 从发光体中心沿 emission direction，拟合前方场景角点，最大视场 178° |
| Spot | 1 | emission direction 为 forward，`tan(acos(outerCosine))` 为投影 extent，投影半角最大为 89° |

点光接收点选择 forward depth 最大的面。Point/Spot 的有限 range 限制 far；没有有限 range 时用场景角点的最大距离确定 far。Rectangle/Disk 的照明不使用 range，因此阴影 far 也忽略该字段，按场景角点最大距离拟合；编辑器仅为 Point/Spot 显示范围控件。方向光使用场景拟合距离，range 不替代其正交场景包络。near 始终为正，透视 near 不超过 0.01 米。Spot 支持大于 0°、不超过 90° 的外半角，并容许 90° 余弦的浮点舍入误差；阴影投影半角限制到 89°，避免 `tan(90°)` 发散，最外侧锥环仍受投影范围限制。

CPU `ProjectShadowPoint` 和 shader 使用相同投影：先在 right/up/forward 基中求坐标；透视除以 `z*tan(halfFov)`，正交除以 half extent。tile UV 的 Y 轴向下，以匹配 D3D viewport；深度保存为 `(z-near)/(far-near)`。投影体外接收点返回未遮挡，因此灯光影响范围和投影覆盖必须保持一致。

## 4. 光栅与 RDG

```text
Scene triangle / material / attributes / texture / ShadowViews upload
  -> Shadow.RasterDepth
       moments RTV + D32 depth test
       draw shared triangles once per shadow view
  -> PCF / PCSS sampling of raw Shadow.Moments

Shadow.RasterDepth
  -> Shadow.BlurHorizontal
  -> Shadow.BlurVertical
  -> VSM sampling
```

光栅使用 RTV slot 7、DSV slot 1。每个 view 设置对应 tile 的 viewport/scissor；全 atlas 先清为 moments `(1,1)`、depth 1。硬件深度使用标准 D3D 透视/正交 clip depth，最近表面通过 D32 深度测试；PS 另行输出线性深度及平方，避免 PCSS 在非线性设备深度上错误估计世界空间 blocker 距离。

`RasterShadowDepth.PSMain` 对 alpha-mask 材质调用共享 `SampleMaterialAlpha(triangle,barycentric)` 并按材质 cutoff 裁剪。PS 按绕序几何法线和朝向光源的方向判断正反面；单面不透明背面被丢弃，double-sided 或 authored transmission 非零的材质保留双面，与射线候选的策略一致。Alpha blend 当前按不透明投影处理，导入器和编辑器不提供此模式；没有分数透射、彩色透射或透明阴影。shadow PSO 关闭硬件背面剔除，由 PS 做材质相关裁剪，使用 D32 和 `R32G32B32A32_FLOAT` moments RTV。

VSM 两个 Compute Pass 分别读取前一级、写独立图像，按完整 atlas 尺寸 dispatch。两个方向均使用归一化九 tap binomial Gaussian，半径 4 texel；每次 Load 都被限制在像素所属 tile。依赖声明覆盖原始矩图、临时图、输出图、常量以及光栅所需的场景资源，不能用 Dispatch 前后隐藏状态切换替代 RDG。

## 5. PCF 与偏移

PCF 在圆盘内分布 16 个确定性 tap，包含中心点，其余沿 golden-angle 分布；逐次比较归一化接收深度与原始深度并求均值。半径由 `shadow.w` 指定，限制到 0..32 texel。它平均的是比较结果，不是先模糊深度再比较。

接收点先沿法线移动 `shadow.z` 米，深度比较再减去 `shadow.y * (1 + 2*slope)` 米，其中 `slope = 1-saturate(dot(normal,incoming))`，`incoming` 为指向光源的单位方向。世界单位偏移转换为归一化深度前除以 `far-near`；偏移过大会产生接触处分离，过小会造成 acne，不能以偏移消除所有分辨率误差。

所有 tap 在当前 view 的整数 texel 范围内 clamp。VSM 手工双线性插值的四个邻点也分别 clamp，因此不会读取相邻光源 tile；当前不在点光 cube face 之间重映射过滤 tap。

## 6. PCSS

PCSS 先以 24 个圆盘 tap（含中心点）搜索位于接收点前方的 blocker。存储线性深度被转换回 view-space 米数，平均有效 blocker 距离为 `z_b`。没有找到 blocker 时返回 1。

透视投影的近似世界空间半影半径为：

```text
r_penumbra = sourceRadius * max(z_receiver-z_blocker,0) / z_blocker
```

方向光则用角半径：

```text
r_penumbra = tan(angularRadius) * max(z_receiver-z_blocker,0)
```

矩形采用最大边长的一半作为 source radius，圆盘使用半径，点/聚光使用 `parameters.x`。世界空间半径用接收深度处的 frustum footprint 转为 texel，再执行 16 tap PCF。blocker 搜索和最终过滤都设最大 32 texel，且至少使用配置的 PCF 半径；这是固定预算近似，不是无限分辨率的物理半影积分。

## 7. VSM

VSM 对线性深度一二阶矩分别做相同的正权重卷积。若接收点在均值前方直接返回 1；否则使用单边 Chebyshev 上界：

```text
variance = max(secondMoment-mean*mean, minimumVariance, 1e-8)
bound = variance / (variance+(receiverDepth-mean)^2)
visibility = saturate((bound-bleedingReduction)/(1-bleedingReduction))
```

最小方差避免退化/浮点抵消，漏光抑制参数被限制到 0..0.999。VSM 的统计上界仍可能漏光，尤其是多个深度层叠；降低漏光并不保证恢复精确 visibility，也可能使软阴影变硬。

## 8. 验证与限制

CPU 测试分五组：方向光 AABB 拟合、点光六面/atlas 分配、透视裁剪、容量/失败原子性、VSM/PCSS 数值边界。独立 MSVC C++20 `/W4 /WX` 编译及 5/5 测试组已通过；`ShadowRenderer.cpp` 通过 `/Zs /W4 /WX` 检查。光栅 VS/PS 与两种后端的水平/垂直 blur 共 6 个 DXC 编译入口通过 `-HV 2021 -WX -O3`。RTX 5070 的 Renderer 集成测试覆盖 24 个 GI/backend/shadow 组合，检查 PSO、RDG 分支和非空输出；额外以确定性的单点光图像确认 PCF/PCSS/VSM 输出两两不同。alpha MASK 与 atlas/cube 边缘仍需要更完整的固定像素对照；当前结果不代表所有输入和全部视角都已验证。最终构建与测试记录见 [`Validation.md`](Validation.md)。

默认 256 texel、10x10 tiles 的 atlas 为 2560x2560：三张 float4 图像共 300 MiB，D32 图像 25 MiB，总计约 325 MiB，不含场景、GBuffer、reservoir 和上传暂存。没有资源别名、缓存静态 shadow tile、方向光 cascade、cube edge tap 重投影或 EVSM/MSM。

矩形/圆盘从中心建立单张透视图，再用 PCSS 近似扩展光源；它没有为每个发光体采样点重新做 visibility。几乎平行于发光平面的射线会受最大视场限制，shadow map 之外返回未遮挡；应使用射线模式进行参考对照。深度偏移、有限分辨率、固定 tap 数、32 texel 上限和 VSM light bleeding 都属于当前质量限制。
