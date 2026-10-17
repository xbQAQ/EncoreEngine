#ifndef RENDER_LIGHT_SAMPLING_HLSLI
#define RENDER_LIGHT_SAMPLING_HLSLI

struct LightSample
{
    float3 direction; // 接收点指向采样发光体的单位方向。
    float3 radiance; // 包含采样位置平方反比衰减的入射辐亮度。
    float distance; // 正距离，方向光使用 MAX_RAY_DISTANCE。
    float pdf; // 光源选择概率乘条件立体角密度，或离散概率质量。
    uint index; // 过滤可见性使用的源光索引。
    uint delta; // 点光、聚光与方向光为一。
    uint valid; // 背面、超范围或空提议为零。
};
float LightArea(Light light)
{
    // 多边形圆盘精确匹配其可见的六十四扇区发光几何。
    float factor = uint(light.positionType.w + 0.5) == 3u ? 8.0 * sin(2.0 * PI / 64.0) : 1.0;
    return max(light.tangentWidth.w * light.bitangentHeight.w * factor, 1e-10);
}
float3 SampleEmitterPoint(Light light, float2 random)
{
    float2 local;
    if (uint(light.positionType.w + 0.5) == 3u) {
        float sector = random.y * 64;
        float angle0 = floor(sector) * (2 * PI / 64), angle1 = angle0 + 2 * PI / 64;
        float2 a = float2(cos(angle0), sin(angle0)), b = float2(cos(angle1), sin(angle1));
        local = sqrt(random.x) * lerp(a, b, frac(sector)) * 0.5;
    } else local = random - 0.5;
    return light.positionType.xyz + light.tangentWidth.xyz * (local.x * light.tangentWidth.w) + light.bitangentHeight.xyz * (local.y * light.bitangentHeight.w);
}
float RangeAttenuation(float distance, float range)
{
    if (range <= 0) return 1;
    float x = distance / range;
    return pow(saturate(1 - x * x * x * x), 2);
}
LightSample SampleLight(float3 position, inout uint state)
{
    LightSample sample = (LightSample)0;
    if (Lighting.x == 0u) return sample;
    sample.index = min(uint(Random(state) * Lighting.x), Lighting.x - 1u);
    Light light = Lights[sample.index];
    uint type = uint(light.positionType.w + 0.5);
    sample.pdf = rcp(float(Lighting.x));
    sample.radiance = max(light.colorIntensity.xyz, 0) * max(light.colorIntensity.w, 0);
    sample.delta = type == 2u || type == 3u ? 0u : 1u;
    if (type == 0u) {
        sample.direction = -light.directionRange.xyz;
        sample.distance = MAX_RAY_DISTANCE;
    } else {
        float3 samplePosition = sample.delta == 0u ? SampleEmitterPoint(light, Random2(state)) : light.positionType.xyz;
        float3 delta = samplePosition - position;
        float distanceSquared = dot(delta, delta);
        sample.distance = sqrt(max(distanceSquared, 1e-12));
        sample.direction = delta / sample.distance;
        if (distanceSquared <= 1e-8) return sample;
        if (sample.delta != 0u) sample.radiance *= rcp(distanceSquared);
        else {
            float cosine = dot(light.directionRange.xyz, -sample.direction);
            if (cosine <= 1e-6) return sample;
            sample.pdf *= distanceSquared / (cosine * LightArea(light));
        }
        if (type == 1u || type == 4u) sample.radiance *= RangeAttenuation(sample.distance, light.directionRange.w);
        if (type == 4u) {
            float cosine = dot(light.directionRange.xyz, -sample.direction);
            float factor = saturate((cosine - light.parameters.y) / max(light.parameters.z - light.parameters.y, 1e-6));
            sample.radiance *= factor * factor;
        }
    }
    sample.valid = MaxComponent(sample.radiance) > 0 ? 1u : 0u;
    return sample;
}
float LightVisibility(LightSample sample, float3 position, float3 geometricNormal)
{
    if (Flags.w == 0u) return 1;
    // 法线贴图改变散射，几何法线决定光线偏移与阴影图接收点偏差。
#if RASTER_ONLY
    // 帧边界选择 PCF、PCSS 或 VSM，表面与参与介质不再追踪可见性光线。
    return FilteredShadowVisibility(sample.index, position, geometricNormal, sample.direction);
#else
    if (Lighting.z != 0u) return FilteredShadowVisibility(sample.index, position, geometricNormal, sample.direction);
    return TraceScene(RayOrigin(position, geometricNormal, sample.direction), sample.direction, sample.distance - 3 * RAY_EPSILON).found == 0u ? 1 : 0;
#endif
}
uint AnalyticEmitterIndex(Hit hit)
{
    // 几何持有发光体身份，共享或编辑材质不得改变光源选择密度。
    if (hit.found == 0u || hit.primitive >= Counts.x) return 0xffffffffu;
    float tag = Triangles[hit.primitive].c.w;
    if (!isfinite(tag) || tag < 1 || tag > float(Lighting.x)) return 0xffffffffu;
    uint index = uint(tag - 1 + 0.5);
    if (tag != float(index + 1u)) return 0xffffffffu;
    uint type = uint(Lights[index].positionType.w + 0.5);
    return type == 2u || type == 3u ? index : 0xffffffffu;
}
float LightPdfSolidAngle(float3 origin, Hit emitter)
{
    uint index = AnalyticEmitterIndex(emitter);
    if (index == 0xffffffffu) return 0;
    Light light = Lights[index];
    float3 delta = emitter.position - origin;
    float cosine = dot(light.directionRange.xyz, -SafeNormalize(delta));
    return cosine > 1e-6 ? dot(delta, delta) / (cosine * LightArea(light) * float(Lighting.x)) : 0;
}
float3 SurfaceEmission(Hit hit, Material material)
{
    // 解析面光源在 NEE、直接可见及路径命中发射中均为单面。
    if (hit.found == 0u || hit.primitive >= Counts.x) return 0;
    if (AnalyticEmitterIndex(hit) != 0xffffffffu && hit.frontFace == 0u) return 0;
    return max(material.emissionMetallic.xyz, 0);
}
float3 DirectLighting(float3 position, float3 normal, float3 geometricNormal, float3 outgoing, Material material, inout uint state, bool useMis)
{
    LightSample sample = SampleLight(position, state);
    if (sample.valid == 0u) return 0;
    // 反射与透射均匹配 BSDF 采样器的几何半球拒绝规则。
    if (dot(normal, sample.direction) * dot(geometricNormal, sample.direction) <= 0) return 0;
    float cosine = abs(dot(normal, sample.direction));
    float visibility = LightVisibility(sample, position, geometricNormal);
    float weight = useMis && sample.delta == 0u ? PowerHeuristic(sample.pdf, BsdfPdf(material, normal, outgoing, sample.direction)) : 1;
    return EvaluateBsdf(material, normal, outgoing, sample.direction) * sample.radiance * (cosine * visibility * weight / max(sample.pdf, 1e-10));
}
float3 DirectDiffuseLighting(float3 position, float3 normal, float3 geometricNormal, float3 albedo, inout uint state, bool useMis)
{
    LightSample sample = SampleLight(position, state);
    if (sample.valid == 0u) return 0;
    // 即使着色法线倾斜，漫反射也不能穿过实际三角形。
    if (dot(geometricNormal, sample.direction) <= 0) return 0;
    float cosine = saturate(dot(normal, sample.direction));
    float weight = useMis && sample.delta == 0u ? PowerHeuristic(sample.pdf, cosine / PI) : 1;
    return albedo * sample.radiance * (cosine * LightVisibility(sample, position, geometricNormal) * weight / max(PI * sample.pdf, 1e-10));
}
#endif
