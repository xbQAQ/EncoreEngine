#include "Common.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    // 几何像素保留已积分辐射，天空只填充光栅背景，不叠加到既有表面。
    float4 input = PostInput.Load(int3(pixel, 0));
    bool background = PositionBuffer.Load(int3(pixel, 0)).w <= 0.0;
    Output0[pixel] = background ? float4(SkyRadiance(CameraRay(float2(pixel) + 0.5)), input.w) : input;
}
