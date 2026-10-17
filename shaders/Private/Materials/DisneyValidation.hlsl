// Standalone numerical harness: no scene resources or renderer root signature are required.
static const float PI = 3.14159265358979323846;
struct Material
{
    float4 baseRoughness; // Linear base color and perceptual roughness.
    float4 emissionMetallic; // Emission and metallic amount.
    float4 transmissionIor; // Specular transmission, IOR, clearcoat and coat gloss.
    float4 principled; // Specular, tint, anisotropy and local subsurface approximation.
    float4 sheen; // Sheen amount, tint, thin flatness and diffuse transmission.
    float4 medium; // Beer attenuation color and reference distance.
    uint4 flags; // Thin, double sided, alpha mode and runtime interior bit.
    uint4 textures; // Texture indices unused by this evaluated-material harness.
    uint4 textures2; // Additional texture indices unused by this harness.
    float4 extras; // Anisotropy rotation, normal scale, alpha cutoff and alpha.
    float4 tangentFrame; // Evaluated tangent direction.
};
struct Hit
{
    float3 position; // Irrelevant origin of the local scattering experiment.
    float3 normal; // Face-forward shading normal.
    float3 geometricNormal; // Face-forward geometric normal for hemisphere rejection.
    float3 tangent; // Surface tangent used when the evaluated material has no tangent.
};
struct BsdfSample
{
    float3 direction; // Sampled world-space scattering direction; zero means a null event.
    float3 weight; // Radiance transport throughput including mixture probability.
    float pdf; // Continuous solid-angle density, zero for a discrete event.
    uint delta; // One for a discrete ideal reflection or refraction.
};

// A private deterministic generator keeps the numerical executable independent of Common.hlsli.
float3 SafeNormalize(float3 v) { return v * rsqrt(max(dot(v, v), 1e-30)); }
float Luminance(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }
float Random(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word >> 8u) * (1.0 / 16777216.0);
}

#include "Disney.hlsli"
#include "SurfaceNormals.hlsli"

struct ValidationResult
{
    float4 importance; // Mean sampled throughput RGB and probability of a non-null sample.
    float4 quadrature; // Independent sphere integral RGB and continuous PDF integral.
    float4 error; // Nonfinite count, maximum PDF error, reciprocity error and delta probability.
    float4 metadata; // Material/view case, trial count, null probability and worst reciprocity comparison scale.
};
RWStructuredBuffer<ValidationResult> Results : register(u0);

// Cases isolate lobes as well as mixed, grazing, interior and degenerate configurations.
Material ValidationMaterial(uint index, out float3 outgoing)
{
    Material m = (Material)0;
    m.baseRoughness = float4(0.45, 0.32, 0.18, 0.6);
    m.transmissionIor = float4(0, 1.5, 0, 0.5);
    m.principled = float4(0.5, 0, 0, 0);
    m.sheen = float4(0, 0.5, 0, 0);
    m.medium = float4(1, 1, 1, 0);
    m.extras = float4(0, 1, 0.5, 1);
    m.tangentFrame = float4(1, 0, 0, 0);
    outgoing = SafeNormalize(float3(0.45, 0.2, 1));
    if (index == 0) { m.principled.x = 0; m.baseRoughness.w = 1; }
    if (index == 1) { m.baseRoughness.rgb = 1; m.baseRoughness.w = 0.5; }
    if (index == 2) { m.emissionMetallic.w = 1; m.principled.z = 0.8; }
    if (index == 3) { m.transmissionIor.zw = float2(1, 0.35); }
    if (index == 4 || index == 5)
    {
        m.transmissionIor.x = 1; m.baseRoughness = float4(0.85, 0.94, 1, 0.65);
        if (index == 5) m.flags.w = 1;
    }
    if (index == 6 || index == 7)
    {
        m.transmissionIor.x = 1; m.baseRoughness = float4(1, 1, 1, 0);
        // Exercise the importer/editor IOR upper endpoint with an independent analytic Fresnel check on the CPU.
        if (index == 6) m.transmissionIor.y = 4;
        if (index == 7) { m.flags.w = 1; outgoing = SafeNormalize(float3(1, 0, 0.3)); }
    }
    if (index == 8) { m.emissionMetallic.w = 1; m.baseRoughness.w = 0; }
    if (index == 9) { m.sheen.xy = float2(1, 1); m.principled.x = 0; }
    if (index == 10) { m.principled.w = 1; }
    if (index == 11) { m.flags.x = 1; m.sheen.zw = float2(1, 1); m.principled.x = 0; }
    if (index == 12) { m.flags.x = 1; m.transmissionIor.x = 1; m.baseRoughness.w = 0.75; }
    if (index == 13) { outgoing = SafeNormalize(float3(1, 0, 0.015)); m.principled.z = 0.9; }
    if (index == 14) { m.emissionMetallic.w = 1; m.principled.z = 0.8; m.extras.x = 0.7; }
    if (index == 15) { m.baseRoughness.rgb = 0; m.principled.x = 0; }
    if (index == 16) { m.transmissionIor.x = 0.4; m.emissionMetallic.w = 0.3; m.sheen.x = 0.6; }
    if (index == 17) { m.principled.xy = float2(1, 1); }
    if (index == 18) { m.emissionMetallic.w = 1; m.principled.z = -0.8; }
    if (index == 19) { m.flags.w = 1; m.transmissionIor.x = 1; m.principled.z = 0.7; m.extras.x = 0.4; }
    if (index == 20) { m.transmissionIor.zw = float2(1, 1); m.baseRoughness.w = 0.7; }
    if (index == 21) { m.flags.x = 1; m.sheen.w = 1; m.transmissionIor.x = 0.35; }
    if (index == 22) { m.baseRoughness.w = 0; m.transmissionIor.x = 0.5; }
    if (index == 23) { m.tangentFrame = 0; m.principled.z = 0.8; }
    return m;
}

// Each lane performs an independent batch; the CPU reduces 64 records per case.
[numthreads(64, 1, 1)]
void ValidationMain(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID)
{
    const uint trials = 256;
    uint caseIndex = group.x;
    if (caseIndex >= 24) return;
    float3 outgoing;
    Material m = ValidationMaterial(caseIndex, outgoing);
    Hit hit = (Hit)0;
    hit.normal = hit.geometricNormal = float3(0, 0, 1);
    hit.tangent = float3(1, 0, 0);
    uint state = 137u + caseIndex * 198491317u + lane.x * 6542989u;
    ValidationResult result = (ValidationResult)0;
    // A smooth normal can accept a tangent-space sample that points below the actual triangle plane.
    float3 geometric = float3(0, 1, 0), smoothNormal = SafeNormalize(float3(1, 1, 0));
    float4 surfaceTangent = float4(SafeNormalize(float3(1, -1, 0)), 1);
    float3 mappedNormal = ApplyNormalMap(float3(0.995, 0, 0.1), smoothNormal, surfaceTangent, geometric);
    if (dot(mappedNormal, geometric) <= 0 || !all(isfinite(mappedNormal))) result.error.x += 1;
    float worstReciprocityScale = 1;
    [loop] for (uint i = 0; i < trials; ++i)
    {
        BsdfSample s = SampleBsdf(m, hit, outgoing, state);
        bool valid = dot(s.direction, s.direction) > 0.5;
        result.importance.rgb += s.weight;
        result.importance.w += valid ? 1.0 : 0.0;
        result.error.w += valid && s.delta != 0 ? 1.0 : 0.0;
        bool finiteSample = all(isfinite(s.weight)) && all(isfinite(s.direction)) && isfinite(s.pdf);
        if (!finiteSample || any(s.weight < 0) || s.pdf < 0) result.error.x += 1;
        if (valid && s.delta == 0)
        {
            float pdf = BsdfPdf(m, hit.normal, outgoing, s.direction);
            result.error.y = max(result.error.y, abs(s.pdf - pdf) / max(1e-6, pdf));
        }

        // Uniform sphere quadrature is independent of the BSDF proposal and catches missing density factors.
        float z = 1 - 2 * Random(state);
        float phi = 2 * PI * Random(state);
        float r = sqrt(max(0, 1 - z * z));
        float3 wi = float3(r * cos(phi), r * sin(phi), z);
        float3 f = EvaluateBsdf(m, hit.normal, outgoing, wi);
        float pdfUniform = BsdfPdf(m, hit.normal, outgoing, wi);
        result.quadrature.rgb += f * abs(z) * (4 * PI);
        result.quadrature.w += pdfUniform * (4 * PI);
        if (!all(isfinite(f)) || !isfinite(pdfUniform) || any(f < 0) || pdfUniform < 0) result.error.x += 1;

        // Radiance BTDF reciprocity differs by eta squared; diffuse thin-sheet transmission is reciprocal without it.
        if (z > 0 || (m.flags.x == 0 && m.sheen.w == 0))
        {
            Material reverse = m;
            float3 reverseNormal = hit.normal;
            float etaSquared = 1;
            if (z < 0)
            {
                reverseNormal = -reverseNormal;
                reverse.flags.w ^= 1;
                float eta = (m.flags.w & 1) != 0 ? rcp(m.transmissionIor.y) : m.transmissionIor.y;
                etaSquared = eta * eta;
            }
            float3 fr = EvaluateBsdf(reverse, reverseNormal, wi, outgoing);
            float3 lhs = f * etaSquared;
            // Near the critical angle, float Fresnel cancellation needs a small absolute tolerance as well as a relative one.
            float denominator = max(1e-3, max(max(lhs.x, lhs.y), max(lhs.z, max(max(fr.x, fr.y), fr.z))));
            float reciprocityError = max(max(abs(lhs.x - fr.x), abs(lhs.y - fr.y)), abs(lhs.z - fr.z)) / denominator;
            if (reciprocityError > result.error.z)
            {
                result.error.z = reciprocityError;
                worstReciprocityScale = denominator;
            }
        }
    }
    result.importance /= trials;
    result.quadrature /= trials;
    result.error.w /= trials;
    result.metadata = float4(caseIndex, trials, 1 - result.importance.w, worstReciprocityScale);
    Results[caseIndex * 64 + lane.x] = result;
}
