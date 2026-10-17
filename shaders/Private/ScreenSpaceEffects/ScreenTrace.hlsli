#ifndef RENDER_SCREEN_TRACE_HLSLI
#define RENDER_SCREEN_TRACE_HLSLI
bool ScreenTrace(float3 origin, float3 direction, float maximumDistance, out uint2 hitPixel)
{
    hitPixel = 0u;
    uint steps = clamp(uint(Sampling.y), 8u, 192u);
    float previousDistance = 0.025;
    float thickness = max(Sampling.z, 0.005);
    // 只查询可见 GBuffer，不读取软件 BVH 或硬件加速结构，离屏立即终止。
    [loop] for (uint stepIndex = 1u; stepIndex <= steps; ++stepIndex)
    {
        float fraction = float(stepIndex) / float(steps);
        float rayDistance = 0.025 + maximumDistance * fraction * fraction;
        float3 rayPoint = origin + direction * rayDistance;
        float2 uv;
        float rayDepth;
        if (!ProjectPosition(rayPoint, false, uv, rayDepth)) return false;
        uint2 pixel = min(uint2(uv * Resolution.xy), uint2(Resolution.xy) - 1u);
        float4 surface = PositionBuffer.Load(int3(pixel, 0));
        if (surface.w > 0.0 && distance(surface.xyz, origin) > 0.05)
        {
            float surfaceDepth = dot(surface.xyz - CameraPosition.xyz, CameraForward.xyz);
            float delta = rayDepth - surfaceDepth;
            float stepDepth = abs(dot(direction, CameraForward.xyz)) * (rayDistance - previousDistance);
            float3 normal = NormalBuffer.Load(int3(pixel, 0)).xyz;
            if (delta >= 0.0 && delta < thickness + stepDepth && dot(normal, -direction) > 0.05 &&
                distance(rayPoint, surface.xyz) < thickness + 2.0 * (rayDistance - previousDistance))
            {
                hitPixel = pixel;
                return true;
            }
        }
        previousDistance = rayDistance;
    }
    return false;
}
#endif
