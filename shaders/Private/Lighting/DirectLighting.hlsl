#include "Common.hlsli"
#if RASTER_ONLY
#include "LTC.hlsli"
#endif

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    Receiver receiver = LoadReceiver(pixel);
    if (receiver.valid == 0u) { Output0[pixel] = 0.0; return; }
    float3 radiance = 0.0;
#if RASTER_ONLY
    // 纯光栅直接光采用确定性 LTC 面积分，不消耗路径采样器或每帧随机种子。
    radiance = LtcDirectLighting(receiver);
#else
    uint state = Seed(pixel, 5u);
    uint samples = clamp(uint(Sampling.x), 1u, 16u);
    // 射线路径保留原有随机直接光估计，与路径和蓄水池的 MIS 密度一致。
    [loop] for (uint i = 0u; i < samples; ++i)
        radiance += DirectLighting(receiver.position, receiver.normal, receiver.geometricNormal, receiver.outgoing, receiver.material, state, false);
    radiance /= float(samples);
#endif
    Hit primary = (Hit)0;
    // 自发光归属由真实图元标记决定，不受可编辑材质槽位影响。
    primary.found = 1u;
    primary.primitive = receiver.primitive;
    primary.position = receiver.position;
    primary.material = receiver.materialId;
    primary.frontFace = receiver.material.flags.w == 0u ? 1u : 0u;
    radiance += SurfaceEmission(primary, receiver.material);
    Output0[pixel] = float4(radiance, 1.0);
}
