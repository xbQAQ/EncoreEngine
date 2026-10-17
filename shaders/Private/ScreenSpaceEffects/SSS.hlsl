#include "Common.hlsli"

void DiffuseSubsurface(uint2 pixel, int2 axis)
{
    if (!InBounds(pixel)) return;
    float4 input = PostInput.Load(int3(pixel, 0));
    Receiver receiver = LoadReceiver(pixel);
    if (receiver.valid == 0u) { Output0[pixel] = input; return; }
    float amount = saturate(receiver.material.principled.w * max(ScreenStrength.w, 0.0)) *
        (1.0 - saturate(receiver.material.emissionMetallic.w)) * (1.0 - saturate(receiver.material.transmissionIor.x));
    if (amount <= 0.0) { Output0[pixel] = input; return; }
    float depth = max(dot(receiver.position - CameraPosition.xyz, CameraForward.xyz), 0.05);
    float radius = max(ScreenGeometry.z, 0.001);
    float radiusPixels = clamp(radius * Resolution.y / (2.0 * depth * CameraForward.w), 1.0, 32.0);
    float3 sum = input.xyz;
    float3 total = 1.0;
    // RGB 扩散宽度不同；材质身份、平面距离及法线共同阻止穿越轮廓和物体。
    [unroll] for (int tap = -6; tap <= 6; ++tap)
    {
        if (tap == 0) continue;
        int2 neighbor = int2(pixel) + axis * int(round(float(tap) * radiusPixels / 3.0));
        if (any(neighbor < 0) || any(neighbor >= int2(Resolution.xy))) continue;
        float4 position = PositionBuffer.Load(int3(neighbor, 0));
        float3 delta = position.xyz - receiver.position;
        float3 normal = NormalBuffer.Load(int3(neighbor, 0)).xyz;
        if (abs(position.w - float(receiver.materialId + 1u)) > 0.25 ||
            abs(dot(delta, receiver.normal)) > radius * 0.35 || dot(receiver.normal, normal) < 0.8) continue;
        float x = float(tap) / 3.0;
        float3 weight = exp(-x * x / float3(1.0, 0.45, 0.2));
        sum += PostInput.Load(int3(neighbor, 0)).xyz * weight;
        total += weight;
    }
    Output0[pixel] = float4(lerp(input.xyz, sum / total, amount), input.w);
}
[numthreads(8, 8, 1)]
void HorizontalMain(uint3 dispatchId : SV_DispatchThreadID) { DiffuseSubsurface(dispatchId.xy, int2(1, 0)); }
[numthreads(8, 8, 1)]
void VerticalMain(uint3 dispatchId : SV_DispatchThreadID) { DiffuseSubsurface(dispatchId.xy, int2(0, 1)); }
