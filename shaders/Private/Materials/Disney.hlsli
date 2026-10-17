#ifndef RENDER_DISNEY_BSDF_HLSLI
#define RENDER_DISNEY_BSDF_HLSLI

// Surface model: Burley 2012/2015 Disney notes; this local SS approximation is not a BSSRDF.
// https://www.disneyanimation.com/publications/physically-based-shading-at-disney/
// Walter dielectric Jacobian: https://pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF
// Exact anisotropic visible-normal sampling: https://jcgt.org/published/0007/04/01/
// Requires evaluated Material, Hit, BsdfSample, SafeNormalize, Luminance and Random.
static const float DisneyPi = 3.14159265358979323846;
static const float DisneyDeltaRoughness = 0.001;

struct DisneyFrame
{
    float3 tangent; // Rotated material-space anisotropy axis.
    float3 bitangent; // Complementary face-forward tangent axis.
    float3 normal; // Shading normal oriented toward the outgoing ray.
};
struct DisneyParameters
{
    float3 color; // Nonnegative linear surface reflectance.
    float3 tint; // Unit-luminance base hue used by specular tint and sheen.
    float3 transmissionColor; // Square-root tint for the two-interface Disney convention.
    float3 dielectricR0; // Artist-controlled opaque dielectric normal-incidence reflectance.
    float metallic; // Blend from dielectric to conductor reflection.
    float specularTransmission; // Dielectric specular transmission amount.
    float diffuseWeight; // Remaining dielectric diffuse fraction.
    float diffuseTransmission; // Thin diffuse partition, in the Disney range [0, 0.5].
    float roughness; // Perceptual base roughness before squaring.
    float2 alpha; // Anisotropic GGX reflection and thick-transmission widths.
    float2 transmissionAlpha; // IOR-adjusted widths for the Disney thin approximation.
    float etaI; // Refractive index on the outgoing side.
    float etaT; // Refractive index on the transmitted side.
    float eta; // etaT / etaI, used consistently by Snell and the transmission Jacobian.
    float subsurface; // Local Burley subsurface blend, including thin flatness.
    float3 sheenColor; // Colored grazing sheen, including amount and diffuse weight.
    float coat; // GTR1 clearcoat amount with the Disney quarter-strength convention.
    float coatAlpha; // Clearcoat width interpolated from the gloss parameter.
    uint thin; // One for the Disney thin surface approximation, without interior absorption.
    uint smooth; // One when base reflection and transmission are discrete events.
};
struct DisneyProbabilities
{
    float diffuse; // Cosine proposal for diffuse reflection and sheen together.
    float reflection; // Visible-normal proposal or delta event for base reflection.
    float transmission; // Visible-normal proposal or delta event for specular transmission.
    float diffuseTransmission; // Cosine proposal on the opposite thin-sheet hemisphere.
    float coat; // GTR1 normal proposal for the additional clearcoat reflection.
};

// A polynomial avoids pow() domain ambiguity around the grazing-angle endpoints.
float DisneyPow5(float value)
{
    float x = saturate(value);
    float x2 = x * x;
    return x2 * x2 * x;
}
float3 FresnelSchlick(float cosine, float3 f0)
{
    return f0 + (1 - f0) * DisneyPow5(1 - abs(cosine));
}

// This exact unpolarized Fresnel handles both entering and leaving rays, including TIR.
float DielectricFresnel(float cosine, float etaI, float etaT)
{
    float c = clamp(cosine, -1, 1);
    if (c < 0)
    {
        float swapEta = etaI;
        etaI = etaT;
        etaT = swapEta;
        c = -c;
    }
    float sinT2 = (etaI * etaI / (etaT * etaT)) * max(0, 1 - c * c);
    if (sinT2 >= 1) return 1;
    float cosT = sqrt(max(0, 1 - sinT2));
    float parallel = (etaT * c - etaI * cosT) / max(1e-20, etaT * c + etaI * cosT);
    float perpendicular = (etaI * c - etaT * cosT) / max(1e-20, etaI * c + etaT * cosT);
    return saturate(0.5 * (parallel * parallel + perpendicular * perpendicular));
}

// Rotate around the winding-defined normal so front/back shading uses the same physical grain axis.
DisneyFrame DisneyMakeFrame(Material material, float3 normal)
{
    DisneyFrame frame;
    frame.normal = SafeNormalize(normal);
    float3 tangent = material.tangentFrame.xyz;
    tangent -= frame.normal * dot(tangent, frame.normal);
    if (dot(tangent, tangent) < 1e-10)
    {
        float3 reference = abs(frame.normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0);
        tangent = cross(reference, frame.normal);
    }
    tangent = SafeNormalize(tangent);
    float side = (material.flags.w & 1u) != 0 ? -1 : 1;
    float sine, cosine;
    sincos(material.extras.x, sine, cosine);
    frame.tangent = tangent * cosine + cross(frame.normal, tangent) * (sine * side);
    frame.bitangent = cross(frame.normal, frame.tangent);
    return frame;
}
float3 DisneyToLocal(DisneyFrame frame, float3 world)
{
    return float3(dot(frame.tangent, world), dot(frame.bitangent, world), dot(frame.normal, world));
}
float3 DisneyToWorld(DisneyFrame frame, float3 local)
{
    return frame.tangent * local.x + frame.bitangent * local.y + frame.normal * local.z;
}

// Signed anisotropy swaps the long axis; alpha is squared perceptual roughness, not roughness itself.
float2 DisneyAlpha(float roughness, float anisotropy)
{
    float a = clamp(anisotropy, -0.99, 0.99);
    float aspect = sqrt(1 - 0.9 * abs(a));
    float alpha = max(0.0001, roughness * roughness);
    float2 widths = float2(alpha / aspect, alpha * aspect);
    return a < 0 ? widths.yx : widths;
}

// Texture evaluation, alpha coverage, normal mapping and emission remain outside this scattering layer.
DisneyParameters DisneyGetParameters(Material material)
{
    DisneyParameters p;
    p.color = max(material.baseRoughness.rgb, 0);
    float luminance = Luminance(p.color);
    p.tint = luminance > 1e-6 ? p.color / luminance : float3(1, 1, 1);
    p.transmissionColor = sqrt(p.color);
    p.dielectricR0 = saturate(0.08 * max(0, material.principled.x) * lerp(float3(1, 1, 1), p.tint, saturate(material.principled.y)));
    p.metallic = saturate(material.emissionMetallic.w);
    p.specularTransmission = saturate(material.transmissionIor.x);
    p.diffuseWeight = (1 - p.metallic) * (1 - p.specularTransmission);
    p.thin = material.flags.x != 0;
    p.diffuseTransmission = p.thin != 0 ? 0.5 * saturate(material.sheen.w) : 0;
    p.roughness = saturate(material.baseRoughness.w);
    p.alpha = DisneyAlpha(p.roughness, material.principled.z);
    // Match imported IOR limits while keeping the refractive half-vector away from eta == 1 singularities.
    float ior = clamp(material.transmissionIor.y, 1.001, 4);
    bool inside = (material.flags.w & 1u) != 0;
    p.etaI = inside ? ior : 1;
    p.etaT = inside ? 1 : ior;
    p.eta = p.etaT / p.etaI;
    float thinRoughness = saturate((0.65 * ior - 0.35) * p.roughness);
    p.transmissionAlpha = p.thin != 0 ? DisneyAlpha(thinRoughness, material.principled.z) : p.alpha;
    float flatness = p.thin != 0 ? saturate(material.sheen.z) : 0;
    p.subsurface = 1 - (1 - saturate(material.principled.w)) * (1 - flatness);
    p.sheenColor = p.diffuseWeight * max(0, material.sheen.x) * lerp(float3(1, 1, 1), p.tint, saturate(material.sheen.y));
    p.coat = 0.25 * max(0, material.transmissionIor.z);
    p.coatAlpha = lerp(0.1, 0.001, saturate(material.transmissionIor.w));
    p.smooth = p.roughness <= DisneyDeltaRoughness;
    return p;
}

// Apply this only to an interior path segment, once; thin surfaces do not establish a medium.
float3 DisneyAbsorption(Material material, float distance)
{
    if (material.flags.x != 0 || material.medium.w <= 0) return float3(1, 1, 1);
    float3 attenuation = clamp(material.medium.rgb, 1e-6, 1);
    return exp(log(attenuation) * (max(0, distance) / material.medium.w));
}

// Opaque specular follows the artist's R0; the transmitting dielectric uses exact IOR Fresnel.
float3 DisneyReflectionFresnel(DisneyParameters p, float cosine)
{
    float3 opaque = FresnelSchlick(cosine, p.dielectricR0);
    float dielectric = DielectricFresnel(abs(cosine), p.etaI, p.etaT);
    float3 dielectricMix = lerp(opaque, dielectric.xxx, p.specularTransmission);
    return lerp(dielectricMix, FresnelSchlick(cosine, p.color), p.metallic);
}

// The separable Smith model matches Disney's GGX distribution and the VNDF proposal below.
float DisneyGgxD(float3 h, float2 alpha)
{
    if (h.z <= 0) return 0;
    float3 v = float3(h.xy / alpha, h.z);
    float denominator = dot(v, v);
    return rcp(DisneyPi * alpha.x * alpha.y * denominator * denominator);
}
float DisneySmithG1(float3 direction, float2 alpha)
{
    float z = abs(direction.z);
    if (z <= 1e-8) return 0;
    float2 xy = direction.xy * alpha;
    return 2 * z / (z + sqrt(dot(xy, xy) + z * z));
}
float DisneyVisibleNormalPdf(float3 outgoing, float3 h, float2 alpha)
{
    if (outgoing.z <= 1e-8 || dot(outgoing, h) <= 0) return 0;
    return DisneyGgxD(h, alpha) * DisneySmithG1(outgoing, alpha) * dot(outgoing, h) / outgoing.z;
}

// Stretch to an isotropic hemisphere, sample its projected disk, then transform the normal back.
float3 DisneySampleVisibleNormal(float3 outgoing, float2 alpha, float2 random)
{
    float3 stretched = SafeNormalize(float3(alpha * outgoing.xy, outgoing.z));
    float lengthSquared = dot(stretched.xy, stretched.xy);
    float3 axis1 = lengthSquared > 1e-12 ? float3(-stretched.y, stretched.x, 0) * rsqrt(lengthSquared) : float3(1, 0, 0);
    float3 axis2 = cross(stretched, axis1);
    float radius = sqrt(random.x);
    float phi = 2 * DisneyPi * random.y;
    float diskX = radius * cos(phi);
    float diskY = radius * sin(phi);
    float blend = 0.5 * (1 + stretched.z);
    diskY = lerp(sqrt(max(0, 1 - diskX * diskX)), diskY, blend);
    float3 normal = diskX * axis1 + diskY * axis2 + sqrt(max(0, 1 - diskX * diskX - diskY * diskY)) * stretched;
    return SafeNormalize(float3(alpha * normal.xy, max(0, normal.z)));
}

// GTR1 uses its normalized projected NDF, rather than the GGX visible-normal distribution.
float DisneyGtr1D(float cosine, float alpha)
{
    if (cosine <= 0) return 0;
    float a2 = alpha * alpha;
    float cosine2 = saturate(cosine * cosine);
    float denominator = max(0, 1 - cosine2) + a2 * cosine2;
    return (a2 - 1) / (DisneyPi * log(a2) * denominator);
}
float3 DisneySampleGtr1(float alpha, float2 random)
{
    float a2 = alpha * alpha;
    float cosine = sqrt(saturate((1 - pow(a2, 1 - random.x)) / (1 - a2)));
    float sine = sqrt(max(0, 1 - cosine * cosine));
    float phi = 2 * DisneyPi * random.y;
    return float3(sine * cos(phi), sine * sin(phi), cosine);
}
float3 DisneyCosineSample(float2 random)
{
    float radius = sqrt(random.x);
    float phi = 2 * DisneyPi * random.y;
    return float3(radius * cos(phi), radius * sin(phi), sqrt(max(0, 1 - random.x)));
}

// Proposal weights are positive wherever a lobe can contribute, including rough interior transmission near TIR.
// A selected microfacet can produce a null event; its probability is not silently redistributed to other lobes.
DisneyProbabilities DisneyMixture(DisneyParameters p, float3 outgoing)
{
    DisneyProbabilities q;
    float colorEnergy = max(0, Luminance(p.color));
    q.diffuse = p.diffuseWeight * (1 - p.diffuseTransmission) * colorEnergy + max(0, Luminance(p.sheenColor)) * 0.1;
    q.reflection = max(0.02, Luminance(DisneyReflectionFresnel(p, outgoing.z)));
    q.transmission = (1 - p.metallic) * p.specularTransmission * max(0.02, Luminance(p.transmissionColor));
    if (p.smooth != 0 && DielectricFresnel(outgoing.z, p.etaI, p.etaT) >= 1) q.transmission = 0;
    q.diffuseTransmission = p.diffuseWeight * p.diffuseTransmission * colorEnergy;
    q.coat = p.coat * (0.04 + 0.96 * DisneyPow5(1 - outgoing.z) + 0.02);
    float sum = q.diffuse + q.reflection + q.transmission + q.diffuseTransmission + q.coat;
    q.diffuse /= sum;
    q.reflection /= sum;
    q.transmission /= sum;
    q.diffuseTransmission /= sum;
    q.coat /= sum;
    return q;
}

// Disney 2015 splits diffuse and retroreflection; the local fake-SS term comes from Burley 2012.
float3 DisneyDiffuse(DisneyParameters p, float3 outgoing, float3 incoming, float cosineHalf)
{
    float fo = DisneyPow5(1 - outgoing.z);
    float fi = DisneyPow5(1 - incoming.z);
    float diffuse = (1 - 0.5 * fo) * (1 - 0.5 * fi);
    float retroStrength = 2 * p.roughness * cosineHalf * cosineHalf;
    float retro = retroStrength * (fo + fi + fo * fi * (retroStrength - 1));
    float ss90 = p.roughness * cosineHalf * cosineHalf;
    float fresnelSS = lerp(1, ss90, fo) * lerp(1, ss90, fi);
    float fakeSS = 1.25 * (fresnelSS * (rcp(max(1e-7, outgoing.z + incoming.z)) - 0.5) + 0.5);
    float diffuseShape = lerp(diffuse, fakeSS, p.subsurface) + retro;
    float3 base = p.color * (p.diffuseWeight * (1 - p.diffuseTransmission) * diffuseShape / DisneyPi);
    return base + p.sheenColor * DisneyPow5(1 - cosineHalf);
}

// Reflection and transmission share the same half-vector orientation and validity rules in evaluation and PDF.
bool DisneyTransmissionHalf(DisneyParameters p, float3 outgoing, float3 incoming, out float3 h)
{
    float3 sum = outgoing + p.eta * incoming;
    if (dot(sum, sum) < 1e-16) { h = 0; return false; }
    h = SafeNormalize(sum);
    if (h.z < 0) h = -h;
    return dot(outgoing, h) > 0 && dot(incoming, h) < 0;
}

// Delta lobes are distributions and are intentionally absent from continuous BSDF evaluation.
float3 DisneyEvaluateLocal(DisneyParameters p, float3 outgoing, float3 incoming)
{
    if (outgoing.z <= 1e-7 || abs(incoming.z) <= 1e-7) return 0;
    if (incoming.z > 0)
    {
        float3 h = SafeNormalize(outgoing + incoming);
        float cosineHalf = saturate(dot(outgoing, h));
        float3 value = DisneyDiffuse(p, outgoing, incoming, cosineHalf);
        if (p.smooth == 0)
        {
            float d = DisneyGgxD(h, p.alpha);
            float g = DisneySmithG1(outgoing, p.alpha) * DisneySmithG1(incoming, p.alpha);
            value += DisneyReflectionFresnel(p, cosineHalf) * (d * g / (4 * outgoing.z * incoming.z));
        }
        if (p.coat > 0)
        {
            float coatF = 0.04 + 0.96 * DisneyPow5(1 - cosineHalf);
            float coatG = DisneySmithG1(outgoing, float2(0.25, 0.25)) * DisneySmithG1(incoming, float2(0.25, 0.25));
            value += (p.coat * DisneyGtr1D(h.z, p.coatAlpha) * coatF * coatG / (4 * outgoing.z * incoming.z)).xxx;
        }
        return value;
    }

    // The Walter change of measure supplies the refractive half-vector denominator; eta^-2 occurs once for radiance.
    float3 value = p.color * (p.diffuseWeight * p.diffuseTransmission / DisneyPi);
    float3 h;
    if (p.smooth == 0 && p.specularTransmission > 0 && p.metallic < 1 && DisneyTransmissionHalf(p, outgoing, incoming, h))
    {
        float oh = dot(outgoing, h);
        float ih = dot(incoming, h);
        float denominator = ih + oh / p.eta;
        if (abs(denominator) <= 1e-10) return value;
        float fresnel = DielectricFresnel(oh, p.etaI, p.etaT);
        float d = DisneyGgxD(h, p.transmissionAlpha);
        float g = DisneySmithG1(outgoing, p.transmissionAlpha) * DisneySmithG1(incoming, p.transmissionAlpha);
        float factor = abs(ih * oh / (outgoing.z * incoming.z * denominator * denominator));
        value += p.transmissionColor * ((1 - p.metallic) * p.specularTransmission * (1 - fresnel) * d * g * factor / (p.eta * p.eta));
    }
    return value;
}

// Each conditional density is multiplied by the exact probability used by the lobe selector.
float DisneyPdfLocal(DisneyParameters p, DisneyProbabilities q, float3 outgoing, float3 incoming)
{
    if (outgoing.z <= 1e-7 || abs(incoming.z) <= 1e-7) return 0;
    if (incoming.z > 0)
    {
        float3 h = SafeNormalize(outgoing + incoming);
        float oh = dot(outgoing, h);
        float density = q.diffuse * incoming.z / DisneyPi;
        if (oh <= 1e-8) return density;
        if (p.smooth == 0) density += q.reflection * DisneyVisibleNormalPdf(outgoing, h, p.alpha) / (4 * oh);
        density += q.coat * DisneyGtr1D(h.z, p.coatAlpha) * h.z / (4 * oh);
        return density;
    }
    float density = q.diffuseTransmission * abs(incoming.z) / DisneyPi;
    float3 h;
    if (p.smooth == 0 && q.transmission > 0 && DisneyTransmissionHalf(p, outgoing, incoming, h))
    {
        float denominator = dot(incoming, h) + dot(outgoing, h) / p.eta;
        if (abs(denominator) > 1e-10)
        {
            float jacobian = abs(dot(incoming, h)) / (denominator * denominator);
            density += q.transmission * DisneyVisibleNormalPdf(outgoing, h, p.transmissionAlpha) * jacobian;
        }
    }
    return density;
}
float3 EvaluateBsdf(Material material, float3 normal, float3 outgoing, float3 incoming)
{
    DisneyFrame frame = DisneyMakeFrame(material, normal);
    return DisneyEvaluateLocal(DisneyGetParameters(material), DisneyToLocal(frame, outgoing), DisneyToLocal(frame, incoming));
}
float BsdfPdf(Material material, float3 normal, float3 outgoing, float3 incoming)
{
    DisneyFrame frame = DisneyMakeFrame(material, normal);
    DisneyParameters p = DisneyGetParameters(material);
    float3 wo = DisneyToLocal(frame, outgoing);
    if (wo.z <= 1e-7) return 0;
    return DisneyPdfLocal(p, DisneyMixture(p, wo), wo, DisneyToLocal(frame, incoming));
}

// The zero direction represents a null sample, including a microfacet that reflects below the macroscopic surface.
BsdfSample SampleBsdf(Material material, Hit hit, float3 outgoing, inout uint state)
{
    BsdfSample sample = (BsdfSample)0;
    DisneyFrame frame = DisneyMakeFrame(material, hit.normal);
    DisneyParameters p = DisneyGetParameters(material);
    float3 wo = DisneyToLocal(frame, outgoing);
    if (wo.z <= 1e-7) return sample;
    DisneyProbabilities q = DisneyMixture(p, wo);
    float selector = Random(state);
    float2 random = float2(Random(state), Random(state));
    float3 wi = 0;
    float discreteProbability = 0;
    float3 discreteThroughput = 0;
    bool transmitted = false;

    // Diffuse and sheen share a cosine proposal; both specular base events may become discrete at zero roughness.
    if (selector < q.diffuse)
        wi = DisneyCosineSample(random);
    else if (selector < q.diffuse + q.reflection)
    {
        float3 h = p.smooth != 0 ? float3(0, 0, 1) : DisneySampleVisibleNormal(wo, p.alpha, random);
        wi = reflect(-wo, h);
        if (wi.z <= 1e-7) return sample;
        if (p.smooth != 0)
        {
            discreteProbability = q.reflection;
            discreteThroughput = DisneyReflectionFresnel(p, wo.z);
        }
    }
    else if (selector < q.diffuse + q.reflection + q.transmission)
    {
        float3 h = p.smooth != 0 ? float3(0, 0, 1) : DisneySampleVisibleNormal(wo, p.transmissionAlpha, random);
        wi = refract(-wo, h, rcp(p.eta));
        if (dot(wi, wi) < 0.5 || wi.z >= -1e-7) return sample;
        transmitted = true;
        if (p.smooth != 0)
        {
            discreteProbability = q.transmission;
            float fresnel = DielectricFresnel(wo.z, p.etaI, p.etaT);
            discreteThroughput = p.transmissionColor * ((1 - p.metallic) * p.specularTransmission * (1 - fresnel) / (p.eta * p.eta));
        }
    }
    else if (selector < q.diffuse + q.reflection + q.transmission + q.diffuseTransmission)
    {
        wi = DisneyCosineSample(random);
        wi.z = -wi.z;
        transmitted = true;
    }
    else
    {
        float3 h = DisneySampleGtr1(p.coatAlpha, random);
        if (dot(wo, h) <= 0) return sample;
        wi = reflect(-wo, h);
        if (wi.z <= 1e-7) return sample;
    }

    // Reject shading-normal leaks through the actual surface; this remains explicit null probability, without renormalization.
    float3 world = SafeNormalize(DisneyToWorld(frame, wi));
    float3 geometric = hit.geometricNormal;
    if (dot(geometric, geometric) > 0.5)
    {
        float signProduct = dot(outgoing, geometric) * dot(world, geometric);
        if ((!transmitted && signProduct <= 0) || (transmitted && signProduct >= 0)) return sample;
    }
    if (discreteProbability > 0)
    {
        sample.direction = world;
        sample.weight = discreteThroughput / discreteProbability;
        sample.delta = 1;
        return sample;
    }

    // Evaluating the entire mixture here accounts for overlapping diffuse, sheen, coat and microfacet proposals.
    // Use the returned normalized direction for both density and throughput, including very sharp coat lobes.
    wi = DisneyToLocal(frame, world);
    float pdf = DisneyPdfLocal(p, q, wo, wi);
    if (pdf <= 1e-20 || !isfinite(pdf)) return sample;
    float3 weight = DisneyEvaluateLocal(p, wo, wi) * (abs(wi.z) / pdf);
    if (!all(isfinite(weight))) return sample;
    sample.direction = world;
    sample.weight = max(0, weight);
    sample.pdf = pdf;
    return sample;
}

#endif
