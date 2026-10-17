# Disney Surface Scattering

`shaders/Private/Materials/Disney.hlsli` implements an evaluated-material surface BSDF. It follows the Burley 2012 diffuse/subsurface approximation and the 2015 surface extensions. It does not implement spatial subsurface transport, a BSSRDF, a spectral conductor model, or a material node graph.

## Contract

The include requires `Material`, `Hit`, `BsdfSample`, `SafeNormalize`, `Luminance`, and `Random`. The renderer supplies the evaluated material after texture and normal-map sampling. The public functions are `EvaluateBsdf`, `BsdfPdf`, `SampleBsdf`, `FresnelSchlick`, `DielectricFresnel`, and `DisneyAbsorption`.

Directions point away from the scattering point. The shading normal must face the outgoing direction. `flags.w & 1` identifies a ray arriving from inside the material; persistent material data must leave this runtime bit zero. It selects the incident/transmitted indices and preserves the physical anisotropy rotation when the face-forward normal reverses. The sampled reflection/transmission hemisphere is also checked against `Hit.geometricNormal` to prevent a normal map from sending a reflection through the geometric surface.

`SampleBsdf` returns `f * abs(dot(normal, direction)) / pdf` for continuous scattering. Its PDF includes the selected-lobe probabilities and all overlapping proposals. Ideal reflection/refraction has `delta == 1` and `pdf == 0`; the throughput already includes the discrete selection probability. A zero direction is a null event. Callers must not resample null events without changing the normalization.

## Parameter Mapping

| Evaluated parameter | Surface behavior |
| --- | --- |
| `baseRoughness.rgb` | Diffuse tint, conductor Schlick reflectance, square-root specular transmission tint. |
| `baseRoughness.w` | Perceptual roughness; squared for anisotropic GGX widths. Values at or below 0.001 select ideal base reflection/refraction. |
| `emissionMetallic.w` | Blend from dielectric diffuse/transmission to conductor reflection. Emission RGB belongs to the path's emitted-radiance term. |
| `transmissionIor.x` | Removes diffuse and adds exact-Fresnel dielectric transmission/reflection. |
| `transmissionIor.y` | IOR for exact Fresnel, Snell refraction, transmission Jacobian, and thin roughness scaling. |
| `transmissionIor.zw` | Quarter-strength GTR1 clearcoat and its 0.1-to-0.001 gloss width. |
| `principled.xy` | Opaque dielectric normal-incidence reflectance `0.08 * specular`, with base-hue tint. Fully transmitting dielectric Fresnel is determined by IOR. |
| `principled.z` | Anisotropic GGX widths; negative values exchange the principal axes. |
| `principled.w` | Local Burley fake-subsurface blend. No spatial scattering or scattering distance is implied. |
| `sheen.xy` | Colored grazing sheen amount and hue tint. |
| `sheen.z` | Additional local fake-subsurface blend for thin surfaces. |
| `sheen.w` | Thin diffuse transmission partition, from zero to one half of diffuse. |
| `medium.rgb/w` | Beer attenuation color at a reference distance; zero distance disables attenuation. Apply `DisneyAbsorption` once per interior path segment. |
| `flags.x` | Disney thin surface approximation: IOR-scaled transmission roughness, diffuse transmission, and flatness; no persistent interior medium. |
| `tangentFrame.xyz`, `extras.x` | Evaluated tangent and winding-normal anisotropy rotation in radians. |

Double-sided visibility, alpha mode/cutoff/factor, texture references, and normal strength are evaluated by the renderer before calling the BSDF. They do not independently alter the BSDF equations.

## Measures And Transport

Base reflection uses anisotropic GGX with separable Smith masking and Heitz's exact visible-normal proposal. Clearcoat uses its normalized projected GTR1 normal proposal and fixed 0.25 Smith width. Diffuse, retroreflection, and sheen share a cosine proposal. Thin diffuse transmission samples the opposite cosine hemisphere.

For specular transmission, let `eta = etaT / etaI`, `wo` be outgoing, `wi` incoming, and orient `h = normalize(wo + eta * wi)` to the shading normal. With `a = dot(wo,h)` and `b = dot(wi,h)`, the normal-to-direction Jacobian is:

```text
dwh/dwi = abs(b) / (b + a/eta)^2
```

The radiance BTDF is:

```text
T * (1-F) * D(h) * G(wo,wi)
  * abs(a*b / (cosO*cosI*(b+a/eta)^2)) / eta^2
```

The radiance correction `1/eta^2` occurs once, including ideal transmission. For reverse transmission the corresponding reciprocity test is `eta^2 * f(wo,wi) = f_reverse(wi,wo)`. Exiting-glass radiance response can exceed one because this is radiance transport across a change of refractive index.

Lobe selection uses positive energy estimates, independently of the exact microfacet Fresnel outcome. TIR or a reflection below the macro surface can reject a selected microfacet. Consequently the integral of the continuous PDF is the accepted continuous probability, not necessarily one. Adding null probability and discrete probability restores the complete proposal mass.

## Model Limits

The 2015 thin transmission is a single-interface Walter approximation with IOR-scaled roughness, not a simulation of a parallel dielectric slab and its repeated internal reflections. It can bend a ray and retains the interface radiance convention. Thin diffuse transmission is reciprocal without an eta correction, so the mixed thin transmission is excluded from a single eta-weighted reciprocity assertion.

The additive Disney diffuse, sheen, reflection, and clearcoat model does not guarantee strict white-furnace energy conservation. For example, the numerical white dielectric case has red hemispherical response about 1.04. This implementation does not hide that behavior with a throughput clamp. Single-scattering GGX can also lose energy at high roughness; no multiple-scattering compensation is claimed.

Normals supplied by the renderer must be valid. Geometric-normal rejection prevents hemisphere leaks but does not implement an adjoint shading-normal correction for bidirectional transport.

The importer accepts IOR in [1, 4]. Evaluation uses [1.001, 4] to avoid the degenerate refractive half-vector at exactly matched indices; an IOR of exactly one is therefore an approximation. The path integrator tracks Beer attenuation on continuation segments inside closed transmitting surfaces. It assumes air at each dielectric interface and does not solve arbitrary nested refractive indices or apply this absorption to next-event shadow segments.

## Numerical Validation

`DisneyValidation.hlsl:ValidationMain` is independent of scene resources. Dispatch `(24,1,1)` groups with 64 threads per group. Each lane performs 256 trials, for 393216 scattering trials. Root UAV `u0` contains 1536 records of 64 bytes, each four `float4` lanes:

| Lane | Contents |
| --- | --- |
| `importance` | Mean sampled throughput RGB, accepted-sample probability. |
| `quadrature` | Independent uniform-sphere BSDF integral RGB, integrated continuous PDF. |
| `error` | Nonfinite/negative count, maximum public PDF mismatch, reciprocity mismatch, discrete probability. |
| `metadata` | Case index, trial count, null probability, scale of worst reciprocity comparison. |

`Tests/DisneyValidationTests.cpp` runs this shader on hardware or WARP using a standalone D3D12 root UAV and readback. It checks finite/nonnegative values, sample/PDF agreement, reflection and applicable transmission reciprocity, independent integration with batch-estimated statistical tolerance, and analytic mirror/glass/TIR cases. Reciprocity uses a relative tolerance of 0.003 with a 0.001 comparison floor, equivalent to a small absolute tolerance near the critical-angle Fresnel cancellation.

The same dispatch also checks the shared normal-map frame with an adversarial smooth normal and tangent-space perturbation that would otherwise cross the geometric plane. The renderer retains the geometric normal separately for NEE hemisphere checks and visibility offsets.

Discrete energy is checked analytically rather than by uniform quadrature. The sharpest GTR1 case also excludes sphere-integral agreement because 16384 uniform directions do not reliably resolve its narrow peak; its finite, public PDF and reciprocity checks remain active. The executable accepts an optional shader path, otherwise loads `RENDER_SHADER_DIRECTORY/DisneyValidation_test.cso`.

## Primary References

- Brent Burley, [Physically-Based Shading at Disney, 2012](https://www.disneyanimation.com/publications/physically-based-shading-at-disney/).
- Brent Burley, [Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering, 2015](https://blog.selfshadow.com/publications/s2015-shading-course/burley/s2015_pbs_disney_bsdf_notes.pdf).
- Eric Heitz, [Sampling the GGX Distribution of Visible Normals, 2018](https://jcgt.org/published/0007/04/01/).
- Pharr, Jakob, and Humphreys, [PBRT 4e: Rough Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF).
- PBRT authors, [Disney material reference implementation](https://github.com/mmp/pbrt-v3/blob/master/src/materials/disney.cpp).
