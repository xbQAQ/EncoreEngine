#include "Common.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    float4 input = PostInput.Load(int3(pixel, 0));
    Receiver receiver = LoadReceiver(pixel);
    if (receiver.valid == 0u) { Output0[pixel] = input; return; }
    float radius = max(ScreenGeometry.x, 0.001);
    float depth = max(dot(receiver.position - CameraPosition.xyz, CameraForward.xyz), 0.05);
    float radiusPixels = clamp(radius * Resolution.y / (2.0 * depth * CameraForward.w), 1.0, 96.0);
    float occlusion = 0.0;
    // 稳定螺旋邻域累积法线半球内的近场遮挡，世界半径避免视距改变尺度。
    [unroll] for (uint index = 0u; index < 24u; ++index)
    {
        float angle = float(index) * 2.39996323;
        float r = radiusPixels * sqrt((float(index) + 0.5) / 24.0);
        int2 neighbor = int2(pixel) + int2(round(float2(cos(angle), sin(angle)) * r));
        if (any(neighbor < 0) || any(neighbor >= int2(Resolution.xy))) continue;
        float4 position = PositionBuffer.Load(int3(neighbor, 0));
        float3 delta = position.xyz - receiver.position;
        float lengthSquared = dot(delta, delta);
        if (position.w <= 0.0 || lengthSquared < 1e-6 || lengthSquared > radius * radius) continue;
        float distanceToSample = sqrt(lengthSquared);
        float horizon = saturate(dot(receiver.normal, delta / distanceToSample) - 0.06);
        occlusion += horizon * (1.0 - distanceToSample / radius);
    }
    float diffuse = (1.0 - saturate(receiver.material.emissionMetallic.w)) *
        (1.0 - saturate(receiver.material.transmissionIor.x));
    float visibility = saturate(1.0 - (occlusion / 24.0) * max(ScreenStrength.x, 0.0) * 3.0);
    Output0[pixel] = float4(input.xyz * lerp(1.0, visibility, diffuse), input.w);
}
