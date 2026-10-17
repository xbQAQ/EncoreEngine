#include "Common.hlsli"

struct RasterVertex
{
    float4 clip : SV_Position; // 相机裁剪位置及透视深度。
    float3 barycentric : TEXCOORD0; // 用于有界源三角形位置重建的透视校正权重。
    float3 normal : TEXCOORD1; // 作者配置的平滑表面法线。
    nointerpolation uint material : TEXCOORD2; // 共享材质标识。
    float2 uv : TEXCOORD3; // 作者配置的源 UV 坐标。
    nointerpolation float4 tangent : TEXCOORD4; // 世界空间切线及 UV 手性。
    nointerpolation float3 geometric : TEXCOORD5; // 由顶点绕序确定的未翻转几何法线。
    nointerpolation uint primitive : TEXCOORD6; // 当前 BVH 顺序中的图元标识。
};
struct GBufferOutput
{
    float4 position : SV_Target0; // 位置及材质标识加一，零表示背景。
    float4 normal : SV_Target1; // 经过纹理求值的法线及粗糙度。
    float4 base : SV_Target2; // 经过纹理求值的基础色及金属度。
    float4 tangent : SV_Target3; // Disney 各向异性使用的切线 XYZ 和手性。
    float4 uv : SV_Target4; // UV、图元索引加一及绕序正面标志。
};
RasterVertex VSMain(uint vertexId : SV_VertexID)
{
    Triangle primitiveData = Triangles[vertexId / 3u];
    uint corner = vertexId % 3u;
    float3 position = corner == 0u ? primitiveData.a.xyz : corner == 1u ? primitiveData.b.xyz : primitiveData.c.xyz;
    float3 relative = position - CameraPosition.xyz;
    float depth = dot(relative, CameraForward.xyz);
    float nearPlane = Projection.x, farPlane = Projection.y;
    RasterVertex output;
    output.clip = float4(dot(relative, CameraRight.xyz) / (CameraForward.w * CameraRight.w),
        dot(relative, CameraUp.xyz) / CameraForward.w, farPlane * (depth - nearPlane) / (farPlane - nearPlane), depth);
    output.barycentric = corner == 0u ? float3(1, 0, 0) : corner == 1u ? float3(0, 1, 0) : float3(0, 0, 1);
    output.geometric = SafeNormalize(cross(primitiveData.b.xyz - primitiveData.a.xyz, primitiveData.c.xyz - primitiveData.a.xyz));
    output.normal = output.geometric;
    output.uv = 0;
    output.tangent = float4(ToWorld(float3(1, 0, 0), output.normal), 1);
    output.primitive = vertexId / 3u;
    output.material = uint(primitiveData.a.w + 0.5);
    if (primitiveData.b.w >= 1 && primitiveData.b.w <= float(Assets.x)) {
        TriangleAttributes attribute = Attributes[uint(primitiveData.b.w - 1)];
        output.normal = corner == 0u ? attribute.normal0.xyz : corner == 1u ? attribute.normal1.xyz : attribute.normal2.xyz;
        output.uv = corner == 0u ? attribute.uv01.xy : corner == 1u ? attribute.uv01.zw : attribute.uv2.xy;
        output.tangent = attribute.tangent;
    }
    return output;
}
GBufferOutput PSMain(RasterVertex input)
{
    Material authored = GetMaterial(input.material);
    Material material = EvaluateMaterial(authored, input.uv, input.tangent, input.material);
    // 量化的光栅覆盖可能外推到接触边缘之外，仅对边界采样选择附近的内部代表点，
    // 避免丢弃像素产生孔洞，也避免接收点恰好落在其他面的 t=0 平面上。
    float3 barycentric = any(input.barycentric <= 0) ? max(input.barycentric, 1e-6) : input.barycentric;
    float3 position = TrianglePosition(Triangles[input.primitive], barycentric);
    bool frontFace = dot(input.geometric, CameraPosition.xyz - position) >= 0;
    // 双面性属于作者配置的表面属性，光线及阴影候选接受规则共同使用。
    if (!frontFace && authored.flags.y == 0u && authored.transmissionIor.x <= 0) discard;
    if (material.flags.z == 1u) clip(material.extras.w * SampleMaterialTexture(material.textures.x, input.uv).w - material.extras.z);
    float3 normal = SafeNormalize(input.normal);
    if (dot(normal, input.geometric) < 0) normal = -normal;
    if (!frontFace) normal = -normal;
    normal = EvaluateNormal(material, input.uv, normal, input.tangent, frontFace ? input.geometric : -input.geometric);
    GBufferOutput output;
    // 光栅与光线路径使用相同的纹理因子和法线贴图手性。
    output.position = float4(position, float(input.material + 1u));
    output.normal = float4(normal, material.baseRoughness.w);
    output.base = float4(material.baseRoughness.xyz, material.emissionMetallic.w);
    output.tangent = input.tangent;
    output.uv = float4(input.uv, float(input.primitive + 1u), frontFace ? 1 : 0);
    return output;
}
