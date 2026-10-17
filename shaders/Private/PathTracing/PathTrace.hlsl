#include "Common.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    uint state = Seed(pixel, 11u);
    uint samples = clamp(uint(Sampling.x), 1u, 16u);
    float3 sum = 0.0;
    [loop] for (uint i = 0u; i < samples; ++i)
        sum += TracePath(CameraPosition.xyz, CameraRay(float2(pixel) + Random2(state)), clamp(Settings.w, 1u, 16u), state);
    // The host resets accumulated samples whenever camera, lighting, materials or integrator change.
    float historySamples = Flags.x != 0u ? 0.0 : max(Timing.z, 0.0);
    float3 previous = historySamples > 0.0 ? PreviousGI.Load(int3(pixel, 0)).xyz : 0.0;
    float totalSamples = historySamples + float(samples);
    Output0[pixel] = float4((previous * historySamples + sum) / totalSamples, totalSamples);
}
