#include "Common.hlsli"
#include "ScreenTrace.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    float4 input = PostInput.Load(int3(pixel, 0));
    Receiver receiver = LoadReceiver(pixel);
    if (receiver.valid == 0u) { Output0[pixel] = input; return; }
    uint state = Seed(pixel, 73u);
    float3 indirect = 0.0;
    uint samples = max(8u, clamp(uint(Sampling.x), 1u, 16u));
    // 独立余弦屏幕追踪积分只估计漫反射，不再叠加旧 SSGI 同一反弹分量。
    [loop] for (uint index = 0u; index < samples; ++index)
    {
        float3 direction = CosineHemisphere(receiver.normal, Random2(state));
        if (dot(direction, receiver.geometricNormal) <= 0.0) continue;
        uint2 hitPixel;
        float3 radiance = SkyRadiance(direction);
        if (ScreenTrace(receiver.position + receiver.geometricNormal * 0.02, direction, 6.0, hitPixel))
        {
            radiance = Settings.y == 1u ? DirectBuffer.Load(int3(hitPixel, 0)).xyz : PostInput.Load(int3(hitPixel, 0)).xyz;
            Receiver secondary = LoadReceiver(hitPixel);
            Hit hit = (Hit)0;
            hit.found = secondary.valid;
            hit.primitive = secondary.primitive;
            hit.material = secondary.materialId;
            hit.position = secondary.position;
            hit.frontFace = secondary.material.flags.w == 0u ? 1u : 0u;
            // 解析发光面的主表面 NEE 已覆盖其发射，屏幕反弹只保留散射部分。
            if (AnalyticEmitterIndex(hit) != 0xffffffffu)
                radiance = max(radiance - SurfaceEmission(hit, secondary.material), 0.0);
        }
        float3 albedo = saturate(receiver.material.baseRoughness.xyz) * (1.0 - saturate(receiver.material.emissionMetallic.w)) *
            (1.0 - saturate(receiver.material.transmissionIor.x));
        indirect += albedo * max(radiance, 0.0);
    }
    indirect *= max(ScreenStrength.z, 0.0) / float(samples);
    // 已有完整路径或 reservoir 间接光时，仅补正屏幕估计高于现有估计的残差。
    float3 existing = Settings.y == 0u ? input.xyz : CurrentGI.Load(int3(pixel, 0)).xyz * max(Controls.z, 0.0);
    float3 addition = Settings.y == 1u ? indirect : max(indirect - existing, 0.0);
    Output0[pixel] = float4(input.xyz + addition, input.w);
}
