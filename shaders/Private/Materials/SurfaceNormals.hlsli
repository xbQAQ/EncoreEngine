#ifndef RENDER_SURFACE_NORMALS_HLSLI
#define RENDER_SURFACE_NORMALS_HLSLI

// Smooth and mapped normals must stay on the same oriented geometric side as the actual triangle.
float3 ConstrainShadingNormal(float3 candidate, float3 fallback, float3 geometricNormal)
{
    if (dot(candidate, geometricNormal) > 0.01) return candidate;
    return dot(fallback, geometricNormal) > 0.01 ? fallback : geometricNormal;
}

float3 ApplyNormalMap(float3 local, float3 normal, float4 tangent, float3 geometricNormal)
{
    normal = ConstrainShadingNormal(normal, geometricNormal, geometricNormal);
    float3 t = SafeNormalize(tangent.xyz - normal * dot(normal, tangent.xyz));
    if (dot(t, t) < 0.1)
        t = SafeNormalize(cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float3 mapped = SafeNormalize(t * local.x + cross(normal, t) * tangent.w * local.y + normal * local.z);
    return ConstrainShadingNormal(mapped, normal, geometricNormal);
}

#endif
