#pragma once
#include <cstdint>

namespace render {
// 每组 16 字节布局与 HLSL 常量及结构化缓冲保持一致。
struct Float4 {
    float x{}; // GPU 数据组的第一个标量。
    float y{}; // GPU 数据组的第二个标量。
    float z{}; // GPU 数据组的第三个标量。
    float w{}; // 第四个标量或打包材质参数。
};
struct Uint4 {
    std::uint32_t x{}; // GPU 数据组的第一个整数。
    std::uint32_t y{}; // GPU 数据组的第二个整数。
    std::uint32_t z{}; // GPU 数据组的第三个整数。
    std::uint32_t w{}; // GPU 数据组的第四个整数。
};
struct GpuTriangle {
    Float4 a; // 世界坐标，w 以浮点数存储材质槽。
    Float4 b; // 第二个世界坐标顶点，w 为属性索引加一，零使用几何回退。
    Float4 c; // 第三个世界坐标顶点，w 为解析光源索引加一，普通几何为零。
};
struct GpuMaterial {
    Float4 baseRoughness; // 线性基础色与感知粗糙度。
    Float4 emissionMetallic; // 线性自发光辐亮度与金属度。
    Float4 transmissionIor; // 镜面透射、折射率、清漆量与清漆光泽。
    Float4 principled{0.5f, 0, 0, 0}; // 高光强度、高光染色、各向异性与次表面混合量。
    Float4 sheen{0, 0.5f, 0, 0}; // 绒毛量、绒毛染色、薄表面平整度与漫透射。
    Float4 medium{1, 1, 1, 0}; // 线性衰减颜色与衰减距离，零关闭吸收。
    Uint4 flags{}; // 薄表面、双面、Alpha 模式，w 保留用于运行时内部状态。
    Uint4 textures{~0u, ~0u, ~0u, ~0u}; // 基础色、金属粗糙度、法线与自发光纹理视图。
    Uint4 textures2{~0u, ~0u, ~0u, ~0u}; // 透射、清漆、绒毛与高光纹理视图。
    Float4 extras{0, 1, 0.5f, 1}; // 各向异性旋转弧度、法线强度、Alpha 阈值与因子。
    Float4 tangentFrame{}; // 可选作者切线覆盖，XYZ 全零时采用求值后的表面切线。
};
struct GpuTriangleAttributes {
    Float4 normal0; // 首顶点的世界空间平滑法线，w 保存切线手性。
    Float4 normal1; // 第二个顶点的世界空间平滑法线。
    Float4 normal2; // 第三个顶点的世界空间平滑法线。
    Float4 uv01; // 第一和第二顶点的 TEXCOORD_0。
    Float4 uv2; // 第三个顶点的 TEXCOORD_0，ZW 保留。
    Float4 tangent; // 世界空间切线 XYZ 与手性 W。
};
struct GpuTextureView {
    Uint4 image; // 打包 RGBA8 偏移、图像宽高与 U/V 寻址模式。
    Float4 offsetScale{0, 0, 1, 1}; // KHR_texture_transform 的 UV 偏移 XY 与缩放 ZW。
    Float4 rotation{0, 1, 0, 0}; // UV 旋转的正余弦、坐标集与保留值。
};
struct GpuLight {
    Float4 positionType; // 世界坐标 XYZ；光源类型依次为方向、点、矩形、圆盘和聚光。
    Float4 directionRange; // 单位发射方向与有限作用距离，零表示无限。
    Float4 colorIntensity; // 线性发光颜色与辐亮度或强度倍率。
    Float4 tangentWidth; // 局部 X 发光轴与矩形完整宽度或圆盘直径。
    Float4 bitangentHeight; // 局部 Z 发光轴与矩形完整高度。
    Float4 parameters; // 光源半径或角半径、聚光外内锥余弦及保留值。
    Uint4 shadow{0, 0, ~0u, 0}; // 首个阴影视图、视图数量、发光材质槽或 UINT_MAX，以及保留值。
};
struct GpuShadowView {
    Float4 originNear; // 投影原点 XYZ 与正数近裁剪距离。
    Float4 rightFar; // 视图单位右向与远裁剪距离。
    Float4 upExtent; // 单位上向与透视半视场角正切或正交半尺寸。
    Float4 forwardProjection; // 单位前向，w 为一表示透视，零表示正交。
    Uint4 atlas; // 图块像素原点 XY、图块分辨率与所属光源索引。
};
struct GpuBvhNode {
    Float4 minimum; // AABB 下界，w 保留。
    Float4 maximum; // AABB 上界，w 保留。
    Uint4 links; // 左子节点、右子节点、首三角形与数量，数量大于零表示叶节点。
};
struct FrameConstants {
    Float4 cameraPosition; // 当前相机原点。
    Float4 cameraForward; // 相机前向，w 为垂直半视场角正切。
    Float4 cameraRight; // 相机右向，w 为宽高比。
    Float4 cameraUp; // 相机上向。
    Float4 previousPosition; // 重投影使用的上一帧相机原点。
    Float4 previousForward; // 上一帧前向与视场角。
    Float4 previousRight; // 上一帧右向与宽高比。
    Float4 previousUp; // 上一帧相机上向。
    Float4 resolution; // 视口宽高及各自倒数。
    Float4 lightPositionSize; // 旧格式保留通道，解析光源改用独立缓冲。
    Float4 lightColorPower; // 帧边界清零的旧格式保留通道。
    Float4 controls; // 曝光 EV、泛光强度、GI 倍率与保留值。
    Uint4 settings; // 帧序号、GI 模式、求交后端与最大路径深度。
    Uint4 counts; // 三角形、BVH 节点、材质与候选数量。
    Uint4 flags; // 历史重置、时间复用、空间复用与阴影开关。
    Float4 restir; // 历史 M 上限、空间半径、法线余弦阈值与相对深度阈值。
    Float4 projection; // 近远裁剪面及两个保留值。
    Float4 post; // 时间权重、泛光阈值、均匀介质密度与相函数各向异性。
    Float4 sampling; // 每帧采样数、屏幕步数、屏幕厚度与调试视图。
    Float4 timing; // 帧间隔、自动曝光开关、累积采样数与保留值。
    Uint4 lighting; // 启用光源数、阴影视图数、阴影模式与图集列数。
    Float4 shadow; // 阴影图块分辨率、世界深度偏移、法线偏移与 PCF 纹素半径。
    Float4 variance; // VSM 最小方差、漏光抑制与两个保留值。
    Uint4 assets; // 三角形属性数、纹理视图数、打包纹素数与保留值。
    Uint4 screenFlags; // SSAO、SSR、SSIL 与 SSS 的独立启用标志。
    Float4 screenStrength; // SSAO、SSR、SSIL 与 SSS 的非负强度。
    Float4 screenGeometry; // AO 世界半径、SSR 最大距离、SSS 世界扩散半径及保留值。
    Float4 sky; // 天空启用标志、天空辐亮度倍率与保留值。
};
static_assert(sizeof(GpuTriangle) == 48);
static_assert(sizeof(GpuMaterial) == 176);
static_assert(sizeof(GpuTriangleAttributes) == 96);
static_assert(sizeof(GpuTextureView) == 48);
static_assert(sizeof(GpuLight) == 112);
static_assert(sizeof(GpuShadowView) == 80);
static_assert(sizeof(GpuBvhNode) == 48);
static_assert(sizeof(FrameConstants) == 448);
}
