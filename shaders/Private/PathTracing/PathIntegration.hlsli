#ifndef RENDER_PATH_INTEGRATION_HLSLI
#define RENDER_PATH_INTEGRATION_HLSLI

float3 TracePath(float3 origin, float3 direction, uint depthLimit, inout uint state)
{
    float3 result = 0, throughput = 1, previousPosition = origin;
    float previousPdf = 0;
    uint previousDelta = 1u, mediumDepth = 0u;
    Material media[16];
    [loop] for (uint depth = 0u; depth < depthLimit; ++depth) {
        Hit hit = TraceScene(origin, direction, MAX_RAY_DISTANCE);
        if (hit.found == 0u) {
            // 天空仅由 BSDF 路径采样，没有环境 NEE 竞争项，因此未命中权重为一。
            result += throughput * SkyRadiance(direction);
            break;
        }
        Material material = GetHitMaterial(hit);
        if (mediumDepth > 0u) throughput *= DisneyAbsorption(media[mediumDepth - 1u], hit.distance);
        float3 emission = SurfaceEmission(hit, material);
        float emissionWeight = previousDelta != 0u ? 1 : PowerHeuristic(previousPdf, LightPdfSolidAngle(previousPosition, hit));
        result += throughput * emission * emissionWeight;
        float3 outgoing = -direction;
        result += throughput * DirectLighting(hit.position, hit.normal, hit.geometricNormal, outgoing, material, state, depth + 1u < depthLimit);
        if (depth + 1u >= depthLimit) break;
        BsdfSample sample = SampleBsdf(material, hit, outgoing, state);
        if (MaxComponent(sample.weight) <= 0 || !all(isfinite(sample.weight)) || dot(sample.direction, sample.direction) < 0.1) break;
        // 有界介质栈仅在穿透封闭表面时跟踪 Beer 衰减。
        bool transmission = dot(sample.direction, hit.geometricNormal) < 0;
        if (transmission && material.flags.x == 0u && material.transmissionIor.x > 0) {
            if (hit.frontFace != 0u && mediumDepth < 16u) media[mediumDepth++] = material;
            else if (hit.frontFace == 0u && mediumDepth > 0u) --mediumDepth;
        }
        throughput *= sample.weight;
        previousPdf = sample.pdf; previousDelta = sample.delta; previousPosition = hit.position;
        origin = RayOrigin(hit.position, hit.geometricNormal, sample.direction);
        direction = SafeNormalize(sample.direction);
        if (depth >= 3u) {
            float survival = clamp(MaxComponent(throughput), 0.05, 0.95);
            if (Random(state) >= survival) break;
            throughput /= survival;
        }
    }
    return all(isfinite(result)) ? max(result, 0) : 0;
}

// ReSTIR 保存方向无关的次级漫反射辐射，光泽及透射主表面单独追踪路径。
float3 DiffuseOutgoing(Hit firstHit, uint depthLimit, inout uint state)
{
    Hit hit = firstHit;
    float3 result = 0, throughput = 1;
    [loop] for (uint depth = 0u; depth < max(depthLimit, 1u); ++depth) {
        Material material = GetHitMaterial(hit);
        float3 albedo = saturate(material.baseRoughness.xyz) * (1 - saturate(material.emissionMetallic.w)) * (1 - saturate(material.transmissionIor.x));
        // 主表面 NEE 已覆盖解析发光体，未注册的自发光网格没有此类提议。
        if (depth == 0u && AnalyticEmitterIndex(hit) == 0xffffffffu)
            result += throughput * SurfaceEmission(hit, material);
        result += throughput * DirectDiffuseLighting(hit.position, hit.normal, hit.geometricNormal, albedo, state, depth + 1u < depthLimit);
        if (depth + 1u >= depthLimit || MaxComponent(albedo) <= 0) break;
        float3 direction = CosineHemisphere(hit.normal, Random2(state));
        // 被几何边界拒绝的余弦提议保留为空事件，不反复重采样。
        if (dot(direction, hit.geometricNormal) <= 0) break;
        float pdf = saturate(dot(hit.normal, direction)) / PI;
        Hit next = TraceScene(RayOrigin(hit.position, hit.geometricNormal, direction), direction, MAX_RAY_DISTANCE);
        throughput *= albedo;
        if (next.found == 0u) {
            // 次级漫反射路径同样以天空为边界，不把无限远样本伪装成表面积 reservoir。
            result += throughput * SkyRadiance(direction);
            break;
        }
        result += throughput * SurfaceEmission(next, GetHitMaterial(next)) * PowerHeuristic(pdf, LightPdfSolidAngle(hit.position, next));
        hit = next;
    }
    return all(isfinite(result)) ? max(result, 0) : 0;
}
#endif
