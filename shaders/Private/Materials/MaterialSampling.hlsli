#ifndef RENDER_MATERIAL_SAMPLING_HLSLI
#define RENDER_MATERIAL_SAMPLING_HLSLI

#include "SurfaceNormals.hlsli"

// 运行时 DXC 注入作者函数，不修改共享着色器源目录。
#ifdef RENDER_MATERIAL_SCRIPTS
#include <MaterialScripts.generated.hlsli>
#endif

float3 SrgbToLinear(float3 color)
{
    float3 low = color / 12.92, high = pow((color + 0.055) / 1.055, 2.4);
    return float3(color.x <= 0.04045 ? low.x : high.x, color.y <= 0.04045 ? low.y : high.y, color.z <= 0.04045 ? low.z : high.z);
}
int WrapImageCoordinate(int coordinate, uint extent, uint mode)
{
    int size = int(max(extent, 1u));
    if (mode == 1u) return clamp(coordinate, 0, size - 1);
    int period = mode == 2u ? size * 2 : size;
    int wrapped = (coordinate % period + period) % period;
    return mode == 2u && wrapped >= size ? period - 1 - wrapped : wrapped;
}
float4 ReadPackedTexel(TextureView view, int2 pixel)
{
    pixel.x = WrapImageCoordinate(pixel.x, view.image.y, view.image.w & 3u);
    pixel.y = WrapImageCoordinate(pixel.y, view.image.z, (view.image.w >> 2u) & 3u);
    uint index = view.image.x + uint(pixel.y) * view.image.y + uint(pixel.x);
    if (index >= Assets.z) return 1.0;
    uint rgba = PackedTexels[index];
    return float4(rgba & 255u, (rgba >> 8u) & 255u, (rgba >> 16u) & 255u, rgba >> 24u) / 255.0;
}
float4 SampleMaterialTexture(uint index, float2 uv, bool srgb = false)
{
    if (index >= Assets.y) return 1.0;
    TextureView view = TextureViews[index];
    uv *= view.offsetScale.zw;
    uv = float2(view.rotation.y * uv.x - view.rotation.x * uv.y, view.rotation.x * uv.x + view.rotation.y * uv.y) + view.offsetScale.xy;
    uint2 mode = uint2(view.image.w & 3u, (view.image.w >> 2u) & 3u);
    uv.x = mode.x == 1u ? saturate(uv.x) : mode.x == 2u ? frac(uv.x * 0.5) * 2 : frac(uv.x);
    uv.y = mode.y == 1u ? saturate(uv.y) : mode.y == 2u ? frac(uv.y * 0.5) * 2 : frac(uv.y);
    float2 pixel = uv * float2(view.image.yz) - 0.5;
    float2 fraction = frac(pixel);
    int2 base = int2(floor(pixel));
    float4 a = ReadPackedTexel(view, base), b = ReadPackedTexel(view, base + int2(1, 0));
    float4 c = ReadPackedTexel(view, base + int2(0, 1)), d = ReadPackedTexel(view, base + 1);
    // 硬件 sRGB 采样在滤波前解码每个采样点；数据贴图和透明度保持线性。
    if (srgb) { a.xyz = SrgbToLinear(a.xyz); b.xyz = SrgbToLinear(b.xyz); c.xyz = SrgbToLinear(c.xyz); d.xyz = SrgbToLinear(d.xyz); }
    return lerp(lerp(a, b, fraction.x), lerp(c, d, fraction.x), fraction.y);
}
float2 TriangleUV(Triangle primitiveData, float2 barycentric)
{
    if (primitiveData.b.w < 1 || primitiveData.b.w > float(Assets.x)) return 0;
    TriangleAttributes attribute = Attributes[uint(primitiveData.b.w - 1)];
    return attribute.uv01.xy * (1 - barycentric.x - barycentric.y) + attribute.uv01.zw * barycentric.x + attribute.uv2.xy * barycentric.y;
}
float SampleMaterialAlpha(Triangle primitiveData, float2 barycentric)
{
    Material material = GetMaterial(uint(primitiveData.a.w + 0.5));
    return material.extras.w * SampleMaterialTexture(material.textures.x, TriangleUV(primitiveData, barycentric)).w;
}
Material EvaluateMaterial(Material material, float2 uv, float4 tangent, uint materialIndex)
{
    float4 base = SampleMaterialTexture(material.textures.x, uv, true);
    float4 mr = SampleMaterialTexture(material.textures.y, uv);
    material.baseRoughness.xyz *= base.xyz;
    material.baseRoughness.w *= mr.y;
    material.emissionMetallic.w *= mr.z;
    material.emissionMetallic.xyz *= SampleMaterialTexture(material.textures.w, uv, true).xyz;
    material.transmissionIor.x *= SampleMaterialTexture(material.textures2.x, uv).x;
    if (material.textures2.y < Assets.y) {
        float2 coat = SampleMaterialTexture(material.textures2.y, uv).xy;
        material.transmissionIor.z *= coat.x;
        material.transmissionIor.w = 1 - (1 - material.transmissionIor.w) * coat.y;
    }
    if (material.textures2.z < Assets.y) {
        float3 sheen = SampleMaterialTexture(material.textures2.z, uv, true).xyz;
        material.sheen.x *= MaxComponent(sheen);
        material.sheen.y = saturate(material.sheen.y + MaxComponent(sheen) - min(sheen.x, min(sheen.y, sheen.z)));
    }
    if (material.textures2.w < Assets.y) {
        float4 specular = SampleMaterialTexture(material.textures2.w, uv, true);
        float3 color = specular.xyz;
        material.principled.x *= specular.w * Luminance(color);
        material.principled.y = saturate(material.principled.y + MaxComponent(color) - min(color.x, min(color.y, color.z)));
    }
    // 作者配置的非零方向显式覆盖导入或生成的表面切线坐标系。
    if (dot(material.tangentFrame.xyz, material.tangentFrame.xyz) < 1e-8) material.tangentFrame = tangent;
#ifdef RENDER_MATERIAL_SCRIPTS
    Material sampled = material;
    ApplyMaterialScript(materialIndex, material, uv, tangent);
    // 表面脚本只编辑光学因子；可见性、资源绑定和法线贴图保留跨后端一致的作者配置。
    material.flags = sampled.flags;
    material.textures = sampled.textures;
    material.textures2 = sampled.textures2;
    material.extras.yzw = sampled.extras.yzw;
    // 即使实验性脚本输出越界，也将光学因子限制在 BRDF 的有效定义域内。
    material.baseRoughness.xyz = max(material.baseRoughness.xyz, 0);
    material.baseRoughness.w = clamp(material.baseRoughness.w, 0.001, 1);
    material.emissionMetallic.xyz = max(material.emissionMetallic.xyz, 0);
    material.emissionMetallic.w = saturate(material.emissionMetallic.w);
#endif
    return material;
}
float3 EvaluateNormal(Material material, float2 uv, float3 normal, float4 tangent, float3 geometricNormal)
{
    if (material.textures.z >= Assets.y) return ConstrainShadingNormal(normal, geometricNormal, geometricNormal);
    float3 local = SampleMaterialTexture(material.textures.z, uv).xyz * 2 - 1;
    local.xy *= material.extras.y;
    // 半球边界由几何法线决定；合法的切线空间贴图也可能越过插值法线所在的半球。
    return ApplyNormalMap(local, normal, tangent, geometricNormal);
}
Material GetHitMaterial(Hit hit)
{
    Material material = EvaluateMaterial(GetMaterial(hit.material), hit.uv, float4(hit.tangent, hit.handedness), hit.material);
    material.flags.w = hit.frontFace != 0u ? 0u : 1u;
    return material;
}
#endif
