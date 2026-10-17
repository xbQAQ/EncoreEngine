# 阴影、资产、多光源与 Disney 材质扩展

用户已授权完成既定功能，本规格直接落实到现有 RDG、模块目录和 ECS，不改变 Runtime 到 Editor 的依赖方向。

## 数据边界

- GpuMaterial 为 176 字节，包含 Disney 2012 全部表面参数、透射/薄表面扩展、贴图视图索引和运行时切线。
- 保留 48 字节三角形位置流供光栅、BLAS 和软件 BVH 共用，b.w 存属性索引加一、c.w 存解析灯索引加一；零分别选择属性回退和普通非灯几何。额外 96 字节属性包含平滑法线、UV 和切线，BVH 重排不破坏属性及 emitter 关联。
- GpuLight 为 112 字节，支持方向、点、矩形、圆盘、聚光；显式限制 16 个动态解析光源，超限报错。
- SRV 根表扩为 t0..t31。t16 灯光，t17 阴影矩，t18 三角形属性，t19 纹理视图，t20 RGBA8 texels，t21 阴影视图，t22 GBuffer 切线，t23 UV。保留原有 t0..t15。
- FrameConstants 为 384 字节，追加灯光数量、阴影模式、偏移和资产数量；常量上传仍按 512 字节对齐。

## 独立模块

1. AssetImport 使用固定版 cgltf v1.15 解析 .gltf/.glb；图像通过系统 WIC 解码。导入三角网格、节点矩阵、平滑法线、UV/切线、PBR 与相关材质扩展、纹理变换及 punctual lights。对不支持的压缩/动画/节点图明确报告，禁止静默成功。
2. Materials/Disney.hlsli 实现 Burley diffuse/subsurface approximation、sheen、anisotropic GGX、GTR1 clearcoat、dielectric Fresnel、rough transmission、thin/diffuse transmission；采样与混合 PDF 一致，镜面使用 delta 分支。完整表面 BSDF 不等同于体积 BSSRDF。
3. Shadows 生成方向光正交、点光六面、矩形/圆盘/聚光透视视图，光栅写入线性深度一二阶矩。PCF 手工比较；PCSS 搜索 blocker 后估计半影；VSM 分离模糊并做方差下限及漏光抑制。阴影采样限制在当前 atlas tile 内。
4. Lighting 统一打包 ECS 灯光，直接光/路径追踪 MIS/ReSTIR secondary/体渲染消费相同灯光列表。面光几何与采样坐标一致；点/方向/聚光为 delta lights。三种 GI 均消费用户选择的阴影模式；Path Tracing 的 `rays` 模式保留精确射线可见性参考，其 shadow map 模式明确属于有偏滤波近似。

## 交付与验证

编辑器提供阴影模式和参数、多光源创建/删除/编辑、完整材质参数及导入入口；命令行可复现导入和阴影模式。CPU 测试覆盖导入/变换/纹理和灯光视图，GPU 测试覆盖三种 GI、两种求交、三种新阴影、材质 lobes 与纹理。以 D3D12 Debug Layer、真实读回和图像差异验证接入，保留现有测试。
