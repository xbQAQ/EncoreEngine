#ifndef RENDER_COMMON_HLSLI
#define RENDER_COMMON_HLSLI
#ifndef USE_DXR
#define USE_DXR 0
#endif
#ifndef RASTER_ONLY
#define RASTER_ONLY 0
#endif

static const float PI = 3.14159265358979323846;
static const float RAY_EPSILON = 0.001;
static const float MAX_RAY_DISTANCE = 10000.0;

// 每组 16 字节布局与 RenderCore/GpuTypes.h 对齐，所有算法共用这一数据契约。
cbuffer FrameConstants : register(b0)
{
    float4 CameraPosition; // 当前相机原点。
    float4 CameraForward; // 相机前向与垂直半视场角的正切值。
    float4 CameraRight; // 相机右向与视口宽高比。
    float4 CameraUp; // 当前相机上向。
    float4 PreviousPosition; // 上一帧相机原点。
    float4 PreviousForward; // 上一帧前向与半视场角的正切值。
    float4 PreviousRight; // 上一帧右向与宽高比。
    float4 PreviousUp; // 上一帧相机上向。
    float4 Resolution; // 视口宽、高及各自的倒数。
    float4 LightPositionSize; // 旧 Cornell 场景显示元数据，不参与光照求值。
    float4 LightColorPower; // 旧 Cornell 场景的光源显示元数据。
    float4 Controls; // 曝光 EV、泛光强度与间接光增益。
    uint4 Settings; // 帧序号、GI 模式、查询后端与路径深度。
    uint4 Counts; // 三角形、BVH 节点、材质与候选数量。
    uint4 Flags; // 历史重置、时间复用、空间复用与阴影启用标志。
    float4 Restir; // 有效 M 上限、邻域半径、法线与深度阈值。
    float4 Projection; // 相机近裁剪面与远裁剪面的正距离。
    float4 Post; // 历史权重、泛光阈值、介质密度与相函数 g。
    float4 Sampling; // 每帧采样数、屏幕步数、厚度与调试视图。
    float4 Timing; // 帧间隔、自动曝光、累积采样数与保留通道。
    uint4 Lighting; // 光源数、阴影视图数、阴影模式与图集列数。
    float4 Shadow; // 图块分辨率、世界深度偏移、法线偏移与 PCF 半径。
    float4 Variance; // VSM 最小方差与漏光抑制强度。
    uint4 Assets; // 顶点属性、纹理视图与打包纹素数量。
    uint4 ScreenFlags; // SSAO、SSR、SSIL 与 SSS 的独立开关。
    float4 ScreenStrength; // 四项屏幕空间效果的非负强度。
    float4 ScreenGeometry; // AO 世界半径、SSR 最大距离、SSS 世界半径与保留值。
    float4 Sky; // 天空开关、线性辐亮度增益与保留值。
};
struct Triangle
{
    float4 a; // 第一个顶点位置与数值化材质标识。
    float4 b; // 第二个顶点位置与稳定属性标识加一。
    float4 c; // 第三个顶点位置与解析光源索引加一；零表示普通几何。
};
struct BvhNode
{
    float4 minimum; // 世界空间包围盒下界。
    float4 maximum; // 世界空间包围盒上界。
    uint4 links; // 左子节点、右子节点、首个图元与叶节点图元数。
};
struct Material
{
    float4 baseRoughness; // 线性基础色与粗糙度。
    float4 emissionMetallic; // 自发光辐亮度与金属度。
    float4 transmissionIor; // 镜面透射、折射率、清漆量与清漆光泽。
    float4 principled; // 高光、高光染色、各向异性与次表面混合。
    float4 sheen; // 绒毛量、绒毛染色、薄表面平整度与漫透射。
    float4 medium; // RGB 衰减颜色与参考距离。
    uint4 flags; // 薄表面、双面、Alpha 模式与运行时内部标志。
    uint4 textures; // 基础色、金属粗糙度、法线与自发光纹理视图。
    uint4 textures2; // 透射、清漆、绒毛与高光纹理视图。
    float4 extras; // 各向异性旋转、法线强度、Alpha 裁剪阈值与因子。
    float4 tangentFrame; // 求值后的切线 XYZ 与坐标系手性。
};
struct TriangleAttributes
{
    float4 normal0; // 第一个导入平滑法线与坐标系手性。
    float4 normal1; // 第二个导入平滑法线。
    float4 normal2; // 第三个导入平滑法线。
    float4 uv01; // 第一组与第二组顶点 UV。
    float4 uv2; // 第三组顶点 UV 与保留通道。
    float4 tangent; // 世界空间切线与 UV 手性。
};
struct TextureView
{
    uint4 image; // 纹素偏移、图像宽高与打包的寻址模式。
    float4 offsetScale; // UV 偏移与缩放。
    float4 rotation; // UV 旋转正余弦、坐标集与保留通道。
};
struct Light
{
    float4 positionType; // 位置 XYZ；方向光为 0，点光为 1，矩形为 2，圆盘为 3，聚光为 4。
    float4 directionRange; // 发射方向与有限作用距离；零表示无限距离。
    float4 colorIntensity; // 发射的线性颜色与强度。
    float4 tangentWidth; // 宽度轴与完整宽度或直径。
    float4 bitangentHeight; // 高度轴与完整高度或直径。
    float4 parameters; // 光源半径、聚光外内锥余弦与保留通道。
    uint4 shadow; // 首个视图、视图数量、发光材质或 UINT_MAX，以及保留通道。
};
struct ShadowView
{
    float4 originNear; // 投影原点与近裁剪面的正距离。
    float4 rightFar; // 单位右向与远裁剪面距离。
    float4 upExtent; // 单位上向与半尺寸或半视场角正切值。
    float4 forwardProjection; // 前向与透视投影标志。
    uint4 atlas; // 图块像素原点、分辨率与所属光源索引。
};
struct Hit
{
    float distance; // 最近交点的光线距离。
    uint primitive; // 共享图元索引。
    uint material; // 命中材质索引。
    uint found; // 接受三角形交点时为一。
    float3 position; // 世界空间交点。
    float3 normal; // 已定向的插值着色法线。
    uint frontFace; // 进入顶点绕序定义的正面时为一。
    float3 geometricNormal; // 用于安全偏移光线原点的已定向几何法线。
    float3 tangent; // 各向异性反射使用的着色切线。
    float handedness; // UV 坐标系方向。
    float2 uv; // 插值后的原始 UV。
};
struct BsdfSample
{
    float3 direction; // 采样得到的世界空间出射方向。
    float3 weight; // BSDF 乘绝对余弦，再除以完整混合概率密度。
    float pdf; // 连续立体角概率密度。
    uint delta; // 离散镜面或折射事件时为一。
};
struct Receiver
{
    float3 position; // 可见表面的世界空间位置。
    float3 normal; // 求值后的着色法线。
    float3 geometricNormal; // 可见性偏移与半球检查所用的已定向图元法线。
    float3 outgoing; // 朝向相机的方向。
    Material material; // 完成纹理求值后的材质。
    uint materialId; // 用于历史兼容性检查的稳定材质标识。
    uint primitive; // 当前 BVH 顺序中的图元，用于发光体标识与几何定向。
    uint valid; // 背景像素为零。
};
struct Reservoir
{
    float4 secondaryPositionPdf; // 次级表面位置与原始面积候选概率密度。
    float4 secondaryNormalTarget; // 次级法线与选中的标量目标值。
    float4 radianceWeightSum; // 与方向无关的次级辐亮度与 RIS 权重和。
    float4 receiverPositionM; // 原接收点位置与有效候选数量。
    float4 receiverNormalW; // 原接收点法线与归一化权重 W。
    float4 directionDistance; // 选中的连接方向与长度。
};
StructuredBuffer<Triangle> Triangles : register(t0);
StructuredBuffer<BvhNode> BvhNodes : register(t1);
StructuredBuffer<Material> Materials : register(t2);
#if USE_DXR
RaytracingAccelerationStructure SceneTlas : register(t3);
#endif
Texture2D<float4> PositionBuffer : register(t4);
Texture2D<float4> NormalBuffer : register(t5);
Texture2D<float4> BaseBuffer : register(t6);
Texture2D<float4> DirectBuffer : register(t7);
Texture2D<float4> PreviousGI : register(t8);
Texture2D<float4> PreviousPositionBuffer : register(t9);
Texture2D<float4> PreviousNormalBuffer : register(t10);
StructuredBuffer<Reservoir> CurrentReservoir : register(t11);
StructuredBuffer<Reservoir> TemporalReservoir : register(t12);
StructuredBuffer<Reservoir> PreviousReservoir : register(t13);
Texture2D<float4> CurrentGI : register(t14);
Texture2D<float4> PostInput : register(t15);
StructuredBuffer<Light> Lights : register(t16);
Texture2D<float4> ShadowAtlas : register(t17);
StructuredBuffer<TriangleAttributes> Attributes : register(t18);
StructuredBuffer<TextureView> TextureViews : register(t19);
StructuredBuffer<uint> PackedTexels : register(t20);
StructuredBuffer<ShadowView> ShadowViews : register(t21);
Texture2D<float4> TangentBuffer : register(t22);
Texture2D<float4> UVBuffer : register(t23);
// LTC 只由光栅直接光 Pass 声明读取，其他变体不会绑定或访问这些表。
Texture2D<float4> LtcMatrix : register(t24);
Texture2D<float4> LtcAmplitude : register(t25);
RWTexture2D<float4> Output0 : register(u0);
RWTexture2D<float4> Output1 : register(u1);
RWTexture2D<float4> Output2 : register(u2);
RWStructuredBuffer<Reservoir> OutputReservoir : register(u3);
SamplerState LinearClamp : register(s0);
SamplerState PointClamp : register(s1);

float Luminance(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }
float MaxComponent(float3 v) { return max(v.x, max(v.y, v.z)); }
bool InBounds(uint2 p) { return all(p < uint2(Resolution.xy)); }
uint PixelIndex(uint2 p) { return p.y * uint(Resolution.x) + p.x; }
float3 SafeNormalize(float3 v) { return v * rsqrt(max(dot(v, v), 1e-20)); }
uint Hash(uint value)
{
    value = value * 747796405u + 2891336453u;
    uint word = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
    return (word >> 22u) ^ word;
}
uint Seed(uint2 pixel, uint stream) { return Hash(PixelIndex(pixel) ^ Hash(Settings.x + 1u) ^ Hash(stream + 193u)); }
float Random(inout uint state) { state = Hash(state); return float(state >> 8u) * (1.0 / 16777216.0); }
float2 Random2(inout uint state) { return float2(Random(state), Random(state)); }
float3 ToWorld(float3 v, float3 n)
{
    float3 t = SafeNormalize(cross(abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), n));
    return t * v.x + cross(n, t) * v.y + n * v.z;
}
float3 CosineHemisphere(float3 n, float2 r)
{
    float radius = sqrt(r.x), phi = 2 * PI * r.y;
    return ToWorld(float3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1 - r.x))), n);
}
float3 TrianglePosition(Triangle primitiveData, float3 barycentric)
{
    // DXR 在共边附近可选择距离误差内的任一相邻面，重建到精确边上会使
    // 法线偏移后的起点仍贴住另一面，零距离遮挡随硬件实现而丢失。
    // 保留数个浮点有效位的凸权重余量，让起点位于所选面的内部再做法线偏移。
    float3 weights = max(barycentric, 0.00000095367431640625);
    weights /= max(weights.x + weights.y + weights.z, 1e-20);
    float3 position = primitiveData.a.xyz * weights.x + primitiveData.b.xyz * weights.y + primitiveData.c.xyz * weights.z;
    return clamp(position, min(primitiveData.a.xyz, min(primitiveData.b.xyz, primitiveData.c.xyz)),
        max(primitiveData.a.xyz, max(primitiveData.b.xyz, primitiveData.c.xyz)));
}
float3 RayOrigin(float3 p, float3 n, float3 d) { return p + n * (dot(n, d) >= 0 ? RAY_EPSILON : -RAY_EPSILON); }
float PowerHeuristic(float a, float b) { return a * a / max(a * a + b * b, 1e-20); }
Material GetMaterial(uint index) { return Materials[min(index, max(Counts.z, 1u) - 1u)]; }

// 算法按职责拆分；上面的共享数据契约不负责实现渲染管线。
#include "../Materials/MaterialSampling.hlsli"
#include "../RayTracing/SceneQueries.hlsli"
#include "../Materials/Disney.hlsli"
#include "../Shadows/ShadowSampling.hlsli"
#include "../Lighting/LightSampling.hlsli"
#include "../Sky/SkyRadiance.hlsli"
// 纯光栅排列不包含路径积分函数，也不携带隐藏的光线查询依赖。
#if !RASTER_ONLY
#include "../PathTracing/PathIntegration.hlsli"
#endif

bool UsesPathFallback(Material material)
{
    // 蓄水池重连接仅存储漫反射次级辐亮度，无法表示主路径的离散事件或 BTDF 透射。
    // 狭窄光泽波瓣上的有限漫反射重连接不稳定；GTR1 清漆本身也具有尖锐波瓣。
    return material.baseRoughness.w < 0.35 || material.transmissionIor.z > 0 ||
        material.transmissionIor.x > 0 || material.sheen.w > 0;
}

float3 CameraRay(float2 pixel)
{
    float2 screen = pixel * Resolution.zw * 2 - 1;
    return SafeNormalize(CameraForward.xyz + CameraRight.xyz * (screen.x * CameraRight.w * CameraForward.w) - CameraUp.xyz * (screen.y * CameraForward.w));
}
bool ProjectPosition(float3 p, bool previous, out float2 uv, out float depth)
{
    float3 relative = p - (previous ? PreviousPosition.xyz : CameraPosition.xyz);
    float4 forward = previous ? PreviousForward : CameraForward, right = previous ? PreviousRight : CameraRight;
    float3 up = previous ? PreviousUp.xyz : CameraUp.xyz;
    depth = dot(relative, forward.xyz);
    uv = float2(dot(relative, right.xyz), -dot(relative, up)) / max(depth * forward.w, 1e-7) / float2(right.w, 1) * 0.5 + 0.5;
    return depth > Projection.x && all(uv >= 0) && all(uv < 1);
}
Receiver LoadReceiver(uint2 pixel)
{
    Receiver receiver = (Receiver)0;
    float4 position = PositionBuffer.Load(int3(pixel, 0));
    receiver.valid = position.w > 0 ? 1u : 0u;
    if (receiver.valid != 0u) {
        float4 frame = TangentBuffer.Load(int3(pixel, 0)), uv = UVBuffer.Load(int3(pixel, 0));
        receiver.position = position.xyz;
        receiver.normal = SafeNormalize(NormalBuffer.Load(int3(pixel, 0)).xyz);
        receiver.outgoing = SafeNormalize(CameraPosition.xyz - position.xyz);
        receiver.materialId = uint(position.w - 1 + 0.5);
        receiver.material = EvaluateMaterial(GetMaterial(receiver.materialId), uv.xy, frame, receiver.materialId);
        receiver.material.flags.w = uv.w > 0.5 ? 0u : 1u;
        // 图元标识经过插值仍保持稳定，可独立于法线贴图恢复几何边界。
        receiver.primitive = uint(max(uv.z - 1, 0) + 0.5);
        receiver.geometricNormal = receiver.normal;
        if (uv.z > 0 && receiver.primitive < Counts.x) {
            Triangle primitiveData = Triangles[receiver.primitive];
            float3 geometric = SafeNormalize(cross(primitiveData.b.xyz - primitiveData.a.xyz, primitiveData.c.xyz - primitiveData.a.xyz));
            receiver.geometricNormal = uv.w > 0.5 ? geometric : -geometric;
        }
    }
    return receiver;
}
bool CompatibleHistory(Receiver receiver, uint2 previousPixel)
{
    float4 oldPosition = PreviousPositionBuffer.Load(int3(previousPixel, 0)), oldNormal = PreviousNormalBuffer.Load(int3(previousPixel, 0));
    float depth = max(length(receiver.position - CameraPosition.xyz), 1);
    return oldPosition.w > 0 && dot(receiver.normal, oldNormal.xyz) > Restir.z &&
        distance(receiver.position, oldPosition.xyz) < max(0.025, Restir.w * depth) &&
        abs(oldPosition.w - float(receiver.materialId + 1u)) < 0.25;
}
#endif
