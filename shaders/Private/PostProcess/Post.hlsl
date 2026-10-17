#include "Common.hlsli"

float3 SceneRadiance(uint2 pixel)
{
    // Resolve owns hybrid composition and denoising; post only consumes its final scene-linear HDR.
    return PostInput.Load(int3(pixel, 0)).xyz;
}
float3 AcesFit(float3 value)
{
    return saturate((value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14));
}
float3 LinearToSrgb(float3 value)
{
    float3 low = value * 12.92;
    float3 high = 1.055 * pow(max(value, 0.0), 1.0 / 2.4) - 0.055;
    return float3(value.x <= 0.0031308 ? low.x : high.x, value.y <= 0.0031308 ? low.y : high.y, value.z <= 0.0031308 ? low.z : high.z);
}
float PhaseHenyeyGreenstein(float cosine, float anisotropy)
{
    float g = clamp(anisotropy, -0.95, 0.95);
    return (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-6), 1.5));
}
float3 ApplyMedium(uint2 pixel, float3 radiance, inout uint state)
{
    float density = max(Post.z, 0.0);
    if (density <= 0.0) return radiance;
    float4 surface = PositionBuffer.Load(int3(pixel, 0));
    float3 direction = CameraRay(float2(pixel) + 0.5);
    float limit = surface.w > 0.0 ? distance(surface.xyz, CameraPosition.xyz) : 8.0;
    limit = min(limit, 12.0);
    const uint steps = 12u;
    float stepLength = limit / float(steps);
    float3 scatter = 0.0;
    float offset = Random(state);
    [loop] for (uint i = 0u; i < steps; ++i)
    {
        float distanceAlongRay = (float(i) + offset) * stepLength;
        float3 position = CameraPosition.xyz + direction * distanceAlongRay;
        LightSample light = SampleLight(position, state);
        if (light.valid == 0u) continue;
        float lightDistance = light.distance >= MAX_RAY_DISTANCE ? 0 : light.distance;
        float transmittance = exp(-density * (distanceAlongRay + lightDistance));
        float phase = PhaseHenyeyGreenstein(dot(direction, light.direction), Post.w);
        // Volume and surface integration share light selection and the selected shadow algorithm.
        float visibility = LightVisibility(light, position, float3(0, 0, 0));
        scatter += light.radiance * (density * stepLength * transmittance * phase * visibility / max(light.pdf, 1e-10));
    }
    return radiance * exp(-density * limit) + scatter;
}
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    uint state = Seed(pixel, 37u);
    float3 radiance = ApplyMedium(pixel, SceneRadiance(pixel), state);
    float exposure = exp2(Controls.x);
    if (Timing.y > 0.5)
    {
        // A deterministic sparse image meter is a small-scene approximation, not a histogram exposure pass.
        float logSum = 0.0;
        [unroll] for (uint y = 0u; y < 4u; ++y)
            [unroll] for (uint x = 0u; x < 4u; ++x)
            {
                uint2 meterPixel = min(uint2((float2(x, y) + 0.5) / 4.0 * Resolution.xy), uint2(Resolution.xy) - 1u);
                logSum += log2(max(Luminance(SceneRadiance(meterPixel)), 1e-4));
            }
        exposure *= clamp(0.18 / exp2(logSum / 16.0), 0.025, 32.0);
    }
    float3 exposed = max(radiance * exposure, 0.0);
    if (Controls.y > 0.0)
    {
        float3 bloom = 0.0;
        float totalWeight = 0.0;
        // A fixed separable-equivalent 5x5 bright filter is intentionally bounded and resolution independent.
        [unroll] for (int y = -2; y <= 2; ++y)
            [unroll] for (int x = -2; x <= 2; ++x)
            {
                int2 neighbor = clamp(int2(pixel) + int2(x, y) * 3, int2(0, 0), int2(Resolution.xy) - 1);
                float weight = exp(-0.4 * float(x * x + y * y));
                float3 color = SceneRadiance(uint2(neighbor)) * exposure;
                float brightness = MaxComponent(color);
                bloom += color * (max(brightness - max(Post.y, 0.0), 0.0) / max(brightness, 1e-6)) * weight;
                totalWeight += weight;
            }
        exposed += bloom * (Controls.y / max(totalWeight, 1e-6));
    }
    float3 display = AcesFit(exposed);
    // The editor displays this UNORM texture directly, so display encoding happens once here.
    Output0[pixel] = float4(LinearToSrgb(display), 1.0);
}
