# 已批准的 GI 实现接口

用户已批准三种可选 GI 和硬件 / 软件双后端，本文记录当前文件所有权与 GPU 契约；2026-10-16 随材质、资产、多光源和阴影扩展更新。

- 公共结构：`Source/Runtime/RenderCore/Public/render/GpuTypes.h`，C++ 与 `shaders/Private/Common/Common.hlsli` 镜像。
- 根签名：b0 FrameConstants（384 字节）；t0–t31 SRV 表；u0–u7 UAV 表；b1 四个 draw constants；s0 线性 clamp，s1 point clamp。
- t0：48 字节 Triangle（a.w 材质、b.w 属性索引加一、c.w 解析灯索引加一）；t1：48 字节 BVHNode；t2：176 字节 Material；t3：DXR TLAS（仅 USE_DXR 变体）。
- t4：Position+materialId+1；t5：Normal+roughness；t6：Albedo+metallic；t7：DirectLighting。
- t8：PreviousGI；t9：PreviousPosition；t10：PreviousNormal。
- t11：CurrentReservoir；t12：TemporalReservoir；t13：PreviousReservoir；t14：CurrentGI；t15：Post / history input。
- t16：112 字节 Light；t17：ShadowAtlas；t18：96 字节 TriangleAttributes；t19：48 字节 TextureView；t20：RGBA8 packed texels；t21：80 字节 ShadowView；t22：GBuffer tangent；t23：UV.xy、primitiveIndex+1、frontFace。t24–t31 保留。
- u0/u1/u2：输出纹理；u3：输出 Reservoir StructuredBuffer；u4–u7：可选输出。
- Reservoir GPU stride：96 字节，6 个 float4 / uint4 lane；具体字段在 shader 与学习文档共同定义。
- 相机右手坐标，Y-up；Forward 指向场景，clip 深度 0..1。世界单位为米。
- GBuffer 由硬件光栅产生；Path Tracing 是独立相机路径积分；SSGI 只读屏幕空间数据；ReSTIR 重用二次命中及其 outgoing radiance，复用时重新计算接收点贡献与目标密度。
- 当前 reservoir 使用二次表面面积测度，`qA = qOmega * cosSecondary / distanceSquared`。复用保持同一个二次表面点，面积域 shift 的 Jacobian 为 1，不能再次给 merge weight 乘立体角 Jacobian；立体角比值只用于有偏的退化重连拒绝。
- 重用为有偏的实用有限历史版本，报告限制；权重、贡献及可见性均有显式定义，不宣称无偏实现。
- 所有上传、Compute、draw、BLAS/TLAS build、读回经 RDG。CPU 配置与描述符创建不算 GPU Pass。

## 当前模块落点

宿主 Thin PImpl 位于 `Source/Editor/EngineEditor/Private/Dx12Renderer.cpp`，只组合 World、RHI、FrameRenderer、EditorUi 和 CaptureService。算法按 `Source/Runtime/Renderer/Private/{Deferred,Lighting,PathTracing,ScreenSpaceGI,ReSTIRGI,Denoiser,PostProcess}` 分开，HLSL 在 `shaders/Private/` 中使用对应目录。`Renderer` 不依赖 `Editor`。

实际架构与验证范围分别见 [Architecture.md](../../Architecture.md) 和 [Validation.md](../../Validation.md)。
