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
    float roughness = saturate(receiver.material.baseRoughness.w);
    float3 direction = reflect(-receiver.outgoing, receiver.normal);
    float3 reflected = 0.0;
    uint2 hitPixel;
    // 屏幕命中采样当前 HDR，离屏缺失只使用可选天空，绝不回退场景求交。
    if (dot(direction, receiver.geometricNormal) > 0.0 &&
        ScreenTrace(receiver.position + receiver.geometricNormal * 0.02, direction, max(ScreenGeometry.y, 0.05), hitPixel))
    {
        float2 edge = (float2(hitPixel) + 0.5) * Resolution.zw;
        float confidence = saturate(min(min(edge.x, edge.y), min(1.0 - edge.x, 1.0 - edge.y)) * 12.0);
        reflected = PostInput.Load(int3(hitPixel, 0)).xyz * confidence;
    }
    else reflected = SkyRadiance(direction);
    float metallic = saturate(receiver.material.emissionMetallic.w);
    float3 f0 = lerp(0.08 * saturate(receiver.material.principled.x), saturate(receiver.material.baseRoughness.xyz), metallic);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(receiver.normal, receiver.outgoing)), 5.0);
    float strength = max(ScreenStrength.y, 0.0) * (1.0 - roughness) * (1.0 - roughness);
    Output0[pixel] = float4(max(input.xyz + reflected * fresnel * strength, 0.0), input.w);
}
