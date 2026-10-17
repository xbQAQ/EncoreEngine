# 光栅面光源与 LTC

## 使用范围

纯光栅管线枚举所有启用光源。方向光、点光和聚光直接求 Disney BSDF；矩形光与圆盘光采用 LTC 解析积分及确定性材质修正。直接光与帧编号、随机种子和路径追踪的 SPP 无关。

编辑器在光追模式显示 SPP；光栅模式仅在启用 SSGI 时显示“间接光采样”。SSGI、SSIL 等屏幕空间算法仍有自己的有限采样与历史重建，不能由光栅化直接推导出这些效果没有噪声。

## 积分与材质

LTC 将拟合后的 GGX 分布变换为余弦分布，使用球面多边形边积分计算整个面光源的贡献。矩形使用四条边；圆盘以固定六十四边形逼近。处理顺序为几何半球、真实着色半球和 LTC 变换半球裁剪，避免法线贴图使光照越过实际表面。

两张 64 × 64 RGBA16F 表保存逆变换矩阵及振幅/Fresnel 积分，查表坐标为粗糙度与 `sqrt(1 - NoV)`。实现显式处理 GLSL 与 HLSL 矩阵构造的行列差异。

Disney 的全部波瓣不等同于上游各向同性 GGX 拟合。实现分开计算：

- Lambert 基线使用解析积分；Burley 漫反射、局部次表面和 sheen 使用固定 8 × 8 面积节点计算与基线的差额。
- 各向同性 GGX 使用 LTC 解析值，并以固定 128 个 VNDF 节点积分 Disney 与实际 LTC 分布的差额。该控制变量同时补偿形状拟合、Fresnel 和 Smith 差异。
- 各向异性 GGX 不套用各向同性表，使用固定 VNDF 求积。
- 清漆 GTR1 与粗糙 BTDF 使用各自分布的固定 128 个节点，分别保留反射与透射测度。
- 零粗糙镜面和理想折射直接检查反射/Snell 方向与发光面的交点，不用有限面积网格逼近 Dirac 分布。

这些节点完全确定，因此不会随帧闪烁，但仍存在有限求积误差。小光源、狭窄高光及极端材质仍需单独验证；当前实现不承诺所有材质和观察角度的统一误差界。

## 阴影

LTC 积分本身不包含场景遮挡。当前 PCF、PCSS、VSM 使用面光源中心方向的阴影可见度乘完整面光源积分，属于有偏近似；它不等同于对整个发光面的可见性积分。

PCSS 估计接触硬化半影，VSM 使用矩过滤与漏光抑制。关闭间接光、体积和随机屏幕效果后，静止相机的直接光输出应逐像素稳定。固定的离散误差或阴影贴图条纹仍可能存在，不能用时间积累掩盖。

## 模块与 RDG

[`LtcLuts`](../Source/Runtime/Renderer/Private/Lighting/LtcLuts.h) 只拥有两张只读拟合纹理。CMake 从固定版本的 DDS 生成嵌入头，程序运行不需要访问源码树的 LTC 文件。初始化上传的暂存资源、复制访问及最终着色读取状态全部由 RDG 声明和执行。

[`DirectLightingPass.cpp`](../Source/Runtime/Renderer/Private/Lighting/DirectLightingPass.cpp) 显式声明表资源的读取，在自己的入口构造并上传参数。[`LTC.hlsli`](../shaders/Private/Lighting/LTC.hlsli) 实现积分，只在 `RASTER_ONLY` 的直接光变体中使用。Path Tracing 和 ReSTIR 的蒙特卡洛光源抽样不受该改动影响。

## 验证

[`LtcLightingTests.cpp`](../tests/LtcLightingTests.cpp) 执行真实 GPU dispatch/readback，使用独立的 192 × 192 面积积分作为连续分布参考，离散镜面/折射使用解析期望。二十个案例覆盖 Lambert、默认 Disney、粗糙金属、掠射角、偏轴、圆盘、地平线裁剪、背面、远光、清漆、sheen/次表面、各向异性、BTDF、薄漫透射、理想镜面/折射、倾斜法线和近距离宽光源。

[`RasterPipelineTests.cpp`](../tests/RasterPipelineTests.cpp) 比较静止场景的连续帧，并将路径采样预算由 1 改为 16，要求纯光栅直接光的显示读回逐像素相同。数值案例与静态稳定性通过并不等于所有场景下无偏或没有离散化误差。实际执行结果见 [`Validation.md`](Validation.md)。

## 数据来源

算法为 Heitz、Dupuy、Hill、Neubelt 的 SIGGRAPH 2016 论文 *Real-Time Polygonal-Light Shading with Linearly Transformed Cosines*，数据来自 [selfshadow/ltc_code](https://github.com/selfshadow/ltc_code) 固定提交 `31e5e96b54f98f33098f8503003119ba2231a1c6`。上游许可证和哈希清单保存在 [`ThirdParty/LTC`](../ThirdParty/LTC/SOURCE.md)。
