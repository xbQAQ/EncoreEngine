#include "Common.hlsli"

cbuffer ShadowDrawConstants : register(b1)
{
    uint ShadowViewIndex; // Current atlas tile/view, provided through root parameter 3.
    uint3 ShadowDrawReserved; // Reserved root lanes preserve the shared four-constant draw contract.
};
struct ShadowVertex
{
    float4 position : SV_Position; // Hardware clipping/depth coordinate for this shadow view.
    float viewDepth : TEXCOORD0; // Perspective-correct light-space depth in world meters.
    float2 barycentric : TEXCOORD1; // Triangle b/c weights for shared texture-alpha lookup.
    nointerpolation uint primitive : TEXCOORD2; // Original shared triangle index for the pixel shader.
};

ShadowVertex VSMain(uint vertexId : SV_VertexID)
{
    ShadowVertex output;
    uint primitive = vertexId / 3u;
    uint vertex = vertexId % 3u;
    Triangle primitiveData = Triangles[primitive];
    float3 position = vertex == 0u ? primitiveData.a.xyz : vertex == 1u ? primitiveData.b.xyz : primitiveData.c.xyz;
    ShadowView view = ShadowViews[ShadowViewIndex];
    float3 delta = position - view.originNear.xyz;
    float z = dot(delta, view.forwardProjection.xyz);
    float2 xy = float2(dot(delta, view.rightFar.xyz), dot(delta, view.upExtent.xyz)) / view.upExtent.w;
    float nearPlane = view.originNear.w;
    float farPlane = view.rightFar.w;
    // Hardware depth is perspective-correct; the color target stores a separately interpolated linear depth.
    if (view.forwardProjection.w > 0.5)
        output.position = float4(xy, farPlane * (z - nearPlane) / (farPlane - nearPlane), z);
    else output.position = float4(xy, (z - nearPlane) / (farPlane - nearPlane), 1.0);
    output.viewDepth = z;
    output.barycentric = vertex == 1u ? float2(1, 0) : vertex == 2u ? float2(0, 1) : float2(0, 0);
    output.primitive = primitive;
    return output;
}

float4 PSMain(ShadowVertex input) : SV_Target0
{
    Triangle primitiveData = Triangles[input.primitive];
    Material material = Materials[uint(primitiveData.a.w + 0.5)];
    ShadowView view = ShadowViews[ShadowViewIndex];
    float3 position = primitiveData.a.xyz * (1 - input.barycentric.x - input.barycentric.y) +
        primitiveData.b.xyz * input.barycentric.x + primitiveData.c.xyz * input.barycentric.y;
    float3 normal = cross(primitiveData.b.xyz - primitiveData.a.xyz, primitiveData.c.xyz - primitiveData.a.xyz);
    float3 toLight = view.forwardProjection.w > 0.5 ? view.originNear.xyz - position : -view.forwardProjection.xyz;
    // Match ray candidate sidedness; glass retains both interfaces for reflection and transmission.
    if (material.flags.y == 0u && material.transmissionIor.x <= 0 && dot(normal, toLight) <= 0) discard;
    // Cutout materials share the raster/ray alpha definition; blended transparency remains opaque here.
    if (material.flags.z == 1u) clip(SampleMaterialAlpha(primitiveData, input.barycentric) - material.extras.z);
    float depth = saturate((input.viewDepth - view.originNear.w) / (view.rightFar.w - view.originNear.w));
    return float4(depth, depth * depth, 0, 1);
}
