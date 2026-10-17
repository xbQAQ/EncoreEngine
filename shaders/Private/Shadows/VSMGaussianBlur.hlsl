#include "Common.hlsli"

float4 BlurShadow(uint2 pixel, int2 axis)
{
    uint resolution = max(uint(Shadow.x), 1u);
    int2 tileMinimum = int2(pixel / resolution * resolution);
    int2 tileMaximum = tileMinimum + int(resolution) - 1;
    // The normalized nine-tap binomial Gaussian never samples a different light's tile.
    static const float weights[5] = {70.0 / 256.0, 56.0 / 256.0, 28.0 / 256.0, 8.0 / 256.0, 1.0 / 256.0};
    float2 moments = ShadowAtlas.Load(int3(pixel, 0)).xy * weights[0];
    [unroll] for (int offset = 1; offset <= 4; ++offset)
    {
        int2 before = clamp(int2(pixel) - axis * offset, tileMinimum, tileMaximum);
        int2 after = clamp(int2(pixel) + axis * offset, tileMinimum, tileMaximum);
        moments += (ShadowAtlas.Load(int3(before, 0)).xy + ShadowAtlas.Load(int3(after, 0)).xy) * weights[offset];
    }
    return float4(moments, 0, 1);
}

[numthreads(8, 8, 1)]
void HorizontalMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint width, height;
    ShadowAtlas.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    Output0[dispatchId.xy] = BlurShadow(dispatchId.xy, int2(1, 0));
}

[numthreads(8, 8, 1)]
void VerticalMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint width, height;
    ShadowAtlas.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    Output0[dispatchId.xy] = BlurShadow(dispatchId.xy, int2(0, 1));
}
