#include "Common.hlsli"

float3 UnfilteredSceneRadiance(uint2 pixel)
{
    float3 indirect = CurrentGI.Load(int3(pixel, 0)).xyz;
    // A PT sample already contains the complete camera path; hybrid modes add primary direct once.
    if (Settings.y == 0u || (Settings.y == 2u && uint(Sampling.w) > 0u)) return indirect;
    // Fallback pixels already contain primary emission/direct lighting; adding DirectBuffer would count them twice.
    if (Settings.y == 2u && UsesPathFallback(LoadReceiver(pixel).material)) return indirect;
    return DirectBuffer.Load(int3(pixel, 0)).xyz + indirect * max(Controls.z, 0.0);
}
bool ResolveCompatible(Receiver receiver, uint2 neighborPixel)
{
    float4 position = PositionBuffer.Load(int3(neighborPixel, 0));
    float4 normal = NormalBuffer.Load(int3(neighborPixel, 0));
    float planeDistance = abs(dot(position.xyz - receiver.position, receiver.normal));
    float depth = max(distance(receiver.position, CameraPosition.xyz), 1.0);
    return position.w > 0.0 && abs(position.w - float(receiver.materialId + 1u)) < 0.25 &&
        dot(receiver.normal, normal.xyz) > max(Restir.z, 0.9) &&
        planeDistance < max(0.012, depth * 0.004) &&
        abs(normal.w - receiver.material.baseRoughness.w) < 0.15;
}

[numthreads(8, 8, 1)]
void TemporalMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    float3 current = max(UnfilteredSceneRadiance(pixel), 0.0);
    Receiver receiver = LoadReceiver(pixel);
    // Preserve the independent PT mean and deterministic reservoir diagnostics without another history filter.
    if (Settings.y == 0u || (Settings.y == 2u && uint(Sampling.w) > 0u) || receiver.valid == 0u)
    {
        Output0[pixel] = float4(current, 1.0);
        return;
    }
    if (Settings.y == 2u)
    {
        // World/camera revisions reset all histories, so a static view can accumulate HDR without clipping its mean.
        // Reservoir history remains bounded independently: M is not a radiance sample count.
        float samples = float(clamp(uint(Sampling.x), 1u, 16u));
        float4 history = PostInput.Load(int3(pixel, 0));
        float oldSamples = Flags.x == 0u && CompatibleHistory(receiver, pixel) &&
            all(isfinite(history)) && history.w > 0.0 ? min(history.w, Timing.z) : 0.0;
        float totalSamples = oldSamples + samples;
        float3 mean = oldSamples > 0.0 ? history.xyz + (current - history.xyz) * (samples / totalSamples) : current;
        Output0[pixel] = float4(all(isfinite(mean)) ? max(mean, 0.0) : current, totalSamples);
        return;
    }
    float3 result = current;
    float confidence = 1.0;
    float2 previousUv;
    float previousDepth;
    if (Flags.x == 0u && Flags.y != 0u && ProjectPosition(receiver.position, true, previousUv, previousDepth))
    {
        uint2 previousPixel = min(uint2(previousUv * Resolution.xy), uint2(Resolution.xy) - 1u);
        if (CompatibleHistory(receiver, previousPixel))
        {
            float3 mean = 0.0;
            float3 secondMoment = 0.0;
            float3 minimum = current;
            float3 maximum = current;
            float count = 0.0;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    int2 neighbor = int2(pixel) + int2(x, y);
                    if (any(neighbor < 0) || any(neighbor >= int2(Resolution.xy))) continue;
                    if (!ResolveCompatible(receiver, uint2(neighbor))) continue;
                    float3 value = max(UnfilteredSceneRadiance(uint2(neighbor)), 0.0);
                    mean += value;
                    secondMoment += value * value;
                    minimum = min(minimum, value);
                    maximum = max(maximum, value);
                    count += 1.0;
                }
            mean /= max(count, 1.0);
            float3 deviation = sqrt(max(secondMoment / max(count, 1.0) - mean * mean, 0.0));
            // Neighborhood variance clipping limits stale light while allowing Monte Carlo fluctuations.
            float3 extent = deviation * 1.75 + 0.025;
            float3 lower = max(minimum, mean - extent);
            float3 upper = min(maximum, mean + extent);
            float4 history = PostInput.Load(int3(previousPixel, 0));
            if (all(isfinite(history)) && history.w > 0.0)
            {
                float oldConfidence = min(history.w, 32.0);
                float historyWeight = min(clamp(Post.x, 0.0, 0.97), oldConfidence / (oldConfidence + 1.0));
                result = lerp(current, clamp(history.xyz, lower, max(lower, upper)), historyWeight);
                confidence = min(oldConfidence + 1.0, 32.0);
            }
        }
    }
    Output0[pixel] = float4(all(isfinite(result)) ? max(result, 0.0) : current, confidence);
}

[numthreads(8, 8, 1)]
void SpatialMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    float4 center = PostInput.Load(int3(pixel, 0));
    Receiver receiver = LoadReceiver(pixel);
    if (Settings.y == 0u || (Settings.y == 2u && uint(Sampling.w) > 0u) || receiver.valid == 0u)
    {
        Output0[pixel] = center;
        return;
    }
    // Complete BSDF paths already integrate a pixel footprint; a diffuse neighbor filter would soften specular detail.
    if (Settings.y == 2u && UsesPathFallback(receiver.material))
    {
        Output0[pixel] = center;
        return;
    }
    float3 sum = center.xyz;
    float sumWeights = 1.0;
    float centerLuminance = Luminance(center.xyz);
    float depth = max(distance(receiver.position, CameraPosition.xyz), 1.0);
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0) continue;
            int2 neighbor = int2(pixel) + int2(x, y);
            if (any(neighbor < 0) || any(neighbor >= int2(Resolution.xy))) continue;
            if (!ResolveCompatible(receiver, uint2(neighbor))) continue;
            float3 position = PositionBuffer.Load(int3(neighbor, 0)).xyz;
            float3 normal = NormalBuffer.Load(int3(neighbor, 0)).xyz;
            float3 value = PostInput.Load(int3(neighbor, 0)).xyz;
            float luminanceDifference = abs(Luminance(value) - centerLuminance);
            float spatialWeight = exp(-0.85 * float(x * x + y * y));
            float normalWeight = pow(saturate(dot(receiver.normal, normal)), 32.0);
            float planeWeight = exp(-abs(dot(position - receiver.position, receiver.normal)) / (0.004 * depth + 0.001));
            // A loose radiance edge term suppresses isolated noise without flattening shadow boundaries.
            float radianceWeight = exp(-luminanceDifference / max(0.25, centerLuminance * 0.6));
            float weight = spatialWeight * normalWeight * planeWeight * radianceWeight;
            sum += value * weight;
            sumWeights += weight;
        }
    // Keep temporal confidence local; filtering neighbors must not invent additional time samples.
    Output0[pixel] = float4(max(sum / sumWeights, 0.0), center.w);
}
