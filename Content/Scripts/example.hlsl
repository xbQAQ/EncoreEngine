// 在导入材质的现有纹理因子求值后叠加程序化棋盘染色。
void Surface(inout Material material, float2 uv, float4 tangent)
{
    float checker = fmod(floor(uv.x * 8.0) + floor(uv.y * 8.0), 2.0);
    material.baseRoughness.xyz *= lerp(float3(0.25, 0.55, 1.0), float3(1.0, 0.8, 0.25), abs(checker));
    material.baseRoughness.w = max(material.baseRoughness.w, 0.15);
}
