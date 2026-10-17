#ifndef RENDER_SKY_RADIANCE_HLSLI
#define RENDER_SKY_RADIANCE_HLSLI
float3 SkyRadiance(float3 direction)
{
    if (Sky.x < 0.5) return 0.0;
    // 解析日光天空以线性辐亮度返回，供背景和 BSDF 未命中使用同一边界条件。
    float elevation = saturate(direction.y);
    float3 horizon = float3(0.62, 0.72, 0.88);
    float3 zenith = float3(0.12, 0.30, 0.65);
    float3 sky = lerp(horizon, zenith, sqrt(elevation));
    float3 ground = float3(0.055, 0.065, 0.075);
    sky = lerp(ground, sky, smoothstep(-0.12, 0.03, direction.y));
    float3 sunDirection = normalize(float3(-0.45, 0.75, -0.35));
    float sun = pow(saturate(dot(direction, sunDirection)), 512.0);
    return (sky + float3(1.0, 0.83, 0.58) * (sun * 3.0)) * max(Sky.y, 0.0);
}
#endif
