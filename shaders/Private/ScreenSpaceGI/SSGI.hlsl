#include "Common.hlsli"

// 屏幕行进不调用 TraceScene，离屏仅采用可选天空边界。
bool ScreenHit(float3 origin, float3 direction, out uint2 hitPixel)
{
    hitPixel = 0u;
    uint steps = clamp(uint(Sampling.y), 8u, 192u);
    float previousDistance = 0.025;
    float thickness = max(Sampling.z, 0.005);
    [loop] for (uint stepIndex = 1u; stepIndex <= steps; ++stepIndex)
    {
        float fraction = float(stepIndex) / float(steps);
        float rayDistance = 0.025 + 6.0 * fraction * fraction;
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
            if (delta >= 0.0 && delta < thickness + stepDepth && dot(normal, -direction) > 0.05 && distance(rayPoint, surface.xyz) < thickness + 2.0 * (rayDistance - previousDistance))
            {
                hitPixel = pixel;
                return true;
            }
        }
        previousDistance = rayDistance;
    }
    return false;
}
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    Receiver receiver = LoadReceiver(pixel);
    if (receiver.valid == 0u) { Output0[pixel] = 0.0; return; }
    // SSIL 独立模块接管屏幕漫反射分量，关闭增益时则保留原有 SSGI 行为。
    if (ScreenFlags.z != 0u && ScreenStrength.z > 0.0) { Output0[pixel] = 0.0; return; }
    uint state = Seed(pixel, 17u);
    uint samples = clamp(uint(Sampling.x), 1u, 16u);
    float3 result = 0.0;
    [loop] for (uint i = 0u; i < samples; ++i)
    {
        float3 direction = CosineHemisphere(receiver.normal, Random2(state));
        // 拒绝几何背面提议，同时保持原余弦提议归一化。
        if (dot(direction, receiver.geometricNormal) <= 0) continue;
        uint2 hitPixel;
        if (ScreenHit(receiver.position + receiver.geometricNormal * 0.02, direction, hitPixel))
        {
            Receiver secondary = LoadReceiver(hitPixel);
            Hit secondaryHit = (Hit)0;
            secondaryHit.found = secondary.valid;
            secondaryHit.primitive = secondary.primitive;
            secondaryHit.material = secondary.materialId;
            secondaryHit.position = secondary.position;
            secondaryHit.frontFace = secondary.material.flags.w == 0u ? 1u : 0u;
            // 保留网格发射与散射，只减去主表面 NEE 已覆盖的解析发射。
            float3 outgoingRadiance = DirectBuffer.Load(int3(hitPixel, 0)).xyz;
            if (AnalyticEmitterIndex(secondaryHit) != 0xffffffffu)
                outgoingRadiance = max(outgoingRadiance - SurfaceEmission(secondaryHit, secondary.material), 0);
            float cosine = saturate(dot(receiver.normal, direction));
            float pdf = cosine / PI;
            // 使用可见屏幕的 DirectBuffer 近似命中表面的出射辐射。
            result += outgoingRadiance * EvaluateBsdf(receiver.material, receiver.normal, receiver.outgoing, direction) * cosine / max(pdf, 1e-8);
        }
        else if (Sky.x > 0.5)
        {
            // 纯光栅模式的离屏边界只查询解析天空，不执行任何场景光线查询。
            result += SkyRadiance(direction) * EvaluateBsdf(receiver.material, receiver.normal, receiver.outgoing, direction) * PI;
        }
    }
    result /= float(samples);
    if (Flags.x == 0u && Flags.y != 0u)
    {
        float2 previousUv;
        float previousDepth;
        if (ProjectPosition(receiver.position, true, previousUv, previousDepth))
        {
            uint2 oldPixel = min(uint2(previousUv * Resolution.xy), uint2(Resolution.xy) - 1u);
            if (CompatibleHistory(receiver, oldPixel))
            {
                float3 history = PreviousGI.Load(int3(oldPixel, 0)).xyz;
                // 保守辐射界限约束遮挡解除后残留的屏幕历史光照。
                history = clamp(history, 0.0, result * 6.0 + 0.35);
                result = lerp(result, history, clamp(Post.x, 0.0, 0.97));
            }
        }
    }
    Output0[pixel] = float4(max(result, 0.0), 1.0);
}
