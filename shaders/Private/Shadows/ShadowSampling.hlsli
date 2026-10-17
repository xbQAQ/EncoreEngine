#ifndef RENDER_SHADOW_SAMPLING_HLSLI
#define RENDER_SHADOW_SAMPLING_HLSLI

float2 ShadowDiskTap(uint index, uint count)
{
    // Include the center so a compact blocker cannot disappear between all search taps.
    if (index == 0u) return 0.0;
    float angle = float(index) * 2.399963229728653;
    float sine, cosine;
    sincos(angle, sine, cosine);
    return float2(cosine, sine) * sqrt(float(index) / max(float(count) - 1.0, 1.0));
}

bool ProjectShadowReceiver(ShadowView view, float3 position, out float2 uv, out float viewDepth)
{
    float3 delta = position - view.originNear.xyz;
    viewDepth = dot(delta, view.forwardProjection.xyz);
    float denominator = view.upExtent.w * (view.forwardProjection.w > 0.5 ? viewDepth : 1.0);
    uv = 0.0;
    if (denominator <= 0.0 || viewDepth < view.originNear.w || viewDepth > view.rightFar.w) return false;
    uv = float2(0.5, 0.5) + float2(dot(delta, view.rightFar.xyz), -dot(delta, view.upExtent.xyz)) / (2.0 * denominator);
    return all(isfinite(uv)) && all(uv >= 0.0) && all(uv <= 1.0);
}

int2 ClampShadowTexel(ShadowView view, float2 tilePixel)
{
    int2 minimum = int2(view.atlas.xy);
    int2 maximum = minimum + int(view.atlas.z) - 1;
    return clamp(int2(floor(tilePixel)) + minimum, minimum, maximum);
}

float2 ShadowMomentsBilinear(ShadowView view, float2 uv)
{
    float2 pixel = uv * float(view.atlas.z) - 0.5;
    float2 fraction = frac(pixel);
    float2 base = floor(pixel);
    // Manual bilinear interpolation clamps all four texels to this view, including atlas-edge pixels.
    float2 a = ShadowAtlas.Load(int3(ClampShadowTexel(view, base), 0)).xy;
    float2 b = ShadowAtlas.Load(int3(ClampShadowTexel(view, base + float2(1, 0)), 0)).xy;
    float2 c = ShadowAtlas.Load(int3(ClampShadowTexel(view, base + float2(0, 1)), 0)).xy;
    float2 d = ShadowAtlas.Load(int3(ClampShadowTexel(view, base + 1), 0)).xy;
    return lerp(lerp(a, b, fraction.x), lerp(c, d, fraction.x), fraction.y);
}

float PcfShadow(ShadowView view, float2 uv, float receiverDepth, float radius)
{
    float visibility = 0.0;
    [unroll] for (uint index = 0; index < 16u; ++index)
    {
        float2 pixel = uv * float(view.atlas.z) + ShadowDiskTap(index, 16u) * radius;
        float stored = ShadowAtlas.Load(int3(ClampShadowTexel(view, pixel), 0)).x;
        visibility += receiverDepth <= stored ? 1.0 : 0.0;
    }
    return visibility / 16.0;
}

float ShadowSourceRadius(Light light)
{
    uint type = uint(light.positionType.w + 0.5);
    if (type == 2u) return max(light.tangentWidth.w, light.bitangentHeight.w) * 0.5;
    if (type == 3u) return light.tangentWidth.w * 0.5;
    return max(light.parameters.x, 0.0);
}

float PcssShadow(ShadowView view, Light light, float2 uv, float viewDepth, float receiverDepth)
{
    bool directional = view.forwardProjection.w < 0.5;
    float sourceRadius = ShadowSourceRadius(light);
    float nearPlane = view.originNear.w;
    float interval = max(view.rightFar.w - nearPlane, 1e-6);
    float worldToTexels = float(view.atlas.z) / (2.0 * view.upExtent.w * (directional ? 1.0 : max(viewDepth, nearPlane)));
    float searchWorld = directional ? tan(min(sourceRadius, 1.4)) * max(viewDepth - nearPlane, 0.0)
        : sourceRadius * max(viewDepth - nearPlane, 0.0) / nearPlane;
    // Finite kernels bound cost and avoid a near-light blocker search spanning the whole atlas tile.
    float radiusLimit = min(32.0, max(float(view.atlas.z) * 0.5 - 1.0, 0.0));
    float searchRadius = clamp(max(Shadow.w, searchWorld * worldToTexels), 0.0, radiusLimit);
    float blockerSum = 0.0;
    uint blockers = 0u;
    [unroll] for (uint index = 0; index < 24u; ++index)
    {
        float2 pixel = uv * float(view.atlas.z) + ShadowDiskTap(index, 24u) * searchRadius;
        float stored = ShadowAtlas.Load(int3(ClampShadowTexel(view, pixel), 0)).x;
        if (stored < receiverDepth)
        {
            blockerSum += nearPlane + stored * interval;
            ++blockers;
        }
    }
    if (blockers == 0u) return 1.0;
    float blockerDepth = blockerSum / float(blockers);
    float separation = max(viewDepth - blockerDepth, 0.0);
    float penumbraWorld = directional ? tan(min(sourceRadius, 1.4)) * separation
        : sourceRadius * separation / max(blockerDepth, nearPlane);
    float filterRadius = clamp(max(Shadow.w, penumbraWorld * worldToTexels), 0.0, radiusLimit);
    return PcfShadow(view, uv, receiverDepth, filterRadius);
}

float VsmShadow(ShadowView view, float2 uv, float receiverDepth)
{
    float2 moments = ShadowMomentsBilinear(view, uv);
    if (!all(isfinite(moments)) || receiverDepth <= moments.x) return 1.0;
    float variance = max(max(Variance.x, 1e-8), moments.y - moments.x * moments.x);
    float delta = receiverDepth - moments.x;
    float bound = variance / (variance + delta * delta);
    float bleeding = clamp(Variance.y, 0.0, 0.999);
    return saturate((bound - bleeding) / (1.0 - bleeding));
}

float FilteredShadowVisibility(uint lightIndex, float3 position, float3 normal, float3 incoming)
{
    if (Flags.w == 0u || Lighting.z == 0u || lightIndex >= Lighting.x) return 1.0;
    Light light = Lights[lightIndex];
    uint first = light.shadow.x;
    uint count = light.shadow.y;
    if (count == 0u || first >= Lighting.y || count > Lighting.y - first) return 1.0;
    uint selected = first;
    float3 biasedPosition = position + normal * max(Shadow.z, 0.0);
    if (count == 6u) {
        // Cube faces are selected by maximum forward depth, preserving the ninety-degree point projection.
        float largestDepth = -1e30;
        [unroll] for (uint face = 0; face < 6u; ++face) {
            ShadowView candidate = ShadowViews[first + face];
            float depth = dot(biasedPosition - candidate.originNear.xyz, candidate.forwardProjection.xyz);
            if (depth > largestDepth) { largestDepth = depth; selected = first + face; }
        }
    }
    ShadowView view = ShadowViews[selected];
    float2 uv;
    float viewDepth;
    if (view.atlas.z == 0u || !ProjectShadowReceiver(view, biasedPosition, uv, viewDepth)) return 1.0;
    float slope = 1.0 - saturate(dot(normal, incoming));
    float depthBias = max(Shadow.y, 0.0) * (1.0 + slope * 2.0);
    float receiverDepth = (viewDepth - view.originNear.w - depthBias) / max(view.rightFar.w - view.originNear.w, 1e-6);
    if (Lighting.z == 3u) return VsmShadow(view, uv, receiverDepth);
    if (Lighting.z == 2u) return PcssShadow(view, light, uv, viewDepth, receiverDepth);
    return PcfShadow(view, uv, receiverDepth, clamp(Shadow.w, 0.0, 32.0));
}
#endif
