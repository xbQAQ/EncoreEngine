#include "GltfInternal.h"

#include <algorithm>
#include <array>
#include <limits>

namespace render::gltf {
namespace {
float Unit(float value, const char *description) {
    Require(std::isfinite(value) && value >= 0 && value <= 1, std::string(description) + " must be within [0, 1]");
    return value;
}
float Positive(float value, const char *description) {
    Require(std::isfinite(value) && value >= 0, std::string(description) + " must be finite and nonnegative");
    return value;
}
bool SameMapping(const GpuTextureView &a, const GpuTextureView &b) {
    return a.image.w == b.image.w && a.offsetScale.x == b.offsetScale.x && a.offsetScale.y == b.offsetScale.y &&
           a.offsetScale.z == b.offsetScale.z && a.offsetScale.w == b.offsetScale.w && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z;
}
std::uint32_t Pixel(const Scene &scene, const GpuTextureView &view, unsigned x, unsigned y, unsigned width,
                    unsigned height) {
    const auto px = std::min(view.image.y - 1, static_cast<unsigned>((std::uint64_t(x) * 2 + 1) * view.image.y /
                                                                     (std::uint64_t(width) * 2)));
    const auto py = std::min(view.image.z - 1, static_cast<unsigned>((std::uint64_t(y) * 2 + 1) * view.image.z /
                                                                     (std::uint64_t(height) * 2)));
    return scene.packedTexels[view.image.x + std::size_t(py) * view.image.y + px];
}
std::uint32_t PackedView(ImportContext &context, const cgltf_texture_view &color, const cgltf_texture_view &scalar,
                         unsigned scalarChannel, bool clearcoat) {
    if (!color.texture && !scalar.texture)
        return ~0u;
    const auto colorIndex = TextureView(context, color), scalarIndex = TextureView(context, scalar);
    const auto colorView = colorIndex != ~0u ? context.scene.textureViews[colorIndex] : GpuTextureView{};
    const auto scalarView = scalarIndex != ~0u ? context.scene.textureViews[scalarIndex] : GpuTextureView{};
    Require(colorIndex == ~0u || scalarIndex == ~0u || SameMapping(colorView, scalarView),
            "extension color and scalar textures must share UV transform and wrap modes");
    auto merged = colorIndex != ~0u ? colorView : scalarView;
    const unsigned width = std::max(colorView.image.y, scalarView.image.y),
                   height = std::max(colorView.image.z, scalarView.image.z);
    const std::uint64_t count = std::uint64_t(width) * height;
    Require(count + context.scene.packedTexels.size() <= std::numeric_limits<std::uint32_t>::max(),
            "packed extension map exceeds 32-bit texture offsets");
    const auto offset = static_cast<std::uint32_t>(context.scene.packedTexels.size());
    // Pack independent glTF extension maps into the renderer's shared RG / RGBA channel contract.
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto rgb = colorIndex != ~0u ? Pixel(context.scene, colorView, x, y, width, height) : 0xffffffffu;
            const auto value =
                scalarIndex != ~0u
                    ? ((Pixel(context.scene, scalarView, x, y, width, height) >> (scalarChannel * 8)) & 255u)
                    : 255u;
            context.scene.packedTexels.push_back(clearcoat ? ((rgb & 255u) | (value << 8) | 0xffff0000u)
                                                           : ((rgb & 0x00ffffffu) | (value << 24)));
        }
    merged.image = {offset, width, height, merged.image.w};
    const auto index = static_cast<std::uint32_t>(context.scene.textureViews.size());
    context.scene.textureViews.push_back(merged);
    return index;
}
GpuMaterial Material(ImportContext &context, const cgltf_material &source) {
    Require(!source.has_pbr_specular_glossiness && !source.unlit,
            "specular-glossiness and unlit materials are unsupported; export Principled metallic/roughness materials");
    Require(!source.has_iridescence && !source.has_dispersion, "iridescence and dispersion materials are unsupported");
    // Geometric transmission remains available, while coverage blending needs a separate ordered/coverage rendering
    // path.
    Require(
        source.alpha_mode != cgltf_alpha_mode_blend,
        "alphaMode BLEND is unsupported; use OPAQUE plus KHR_materials_transmission for glass, or MASK for cutouts");
    GpuMaterial result;
    const auto &pbr = source.pbr_metallic_roughness;
    // glTF color factors are already linear; only texture RGB channels are decoded from sRGB in shaders.
    result.baseRoughness = {Unit(pbr.base_color_factor[0], "base color"), Unit(pbr.base_color_factor[1], "base color"),
                            Unit(pbr.base_color_factor[2], "base color"), Unit(pbr.roughness_factor, "roughness")};
    const float strength =
        source.has_emissive_strength ? Positive(source.emissive_strength.emissive_strength, "emissive strength") : 1;
    result.emissionMetallic = {
        Finite(double(Positive(source.emissive_factor[0], "emission")) * strength, "emitted radiance"),
        Finite(double(Positive(source.emissive_factor[1], "emission")) * strength, "emitted radiance"),
        Finite(double(Positive(source.emissive_factor[2], "emission")) * strength, "emitted radiance"),
        Unit(pbr.metallic_factor, "metallic")};
    // Surface extensions map into explicit Disney lanes; unsupported independent lobes are rejected below.
    result.transmissionIor = {
        source.has_transmission ? Unit(source.transmission.transmission_factor, "transmission") : 0,
        source.has_ior ? source.ior.ior : 1.5f,
        source.has_clearcoat ? Unit(source.clearcoat.clearcoat_factor, "clearcoat") : 0,
        source.has_clearcoat ? 1 - Unit(source.clearcoat.clearcoat_roughness_factor, "clearcoat roughness") : 1};
    Require(std::isfinite(result.transmissionIor.y) && result.transmissionIor.y >= 1 && result.transmissionIor.y <= 4,
            "material IOR must be within [1, 4]");
    result.principled.x = source.has_specular ? 0.5f * Unit(source.specular.specular_factor, "specular level") : 0.5f;
    if (source.has_specular) {
        const auto *color = source.specular.specular_color_factor;
        for (unsigned i = 0; i < 3; ++i)
            Unit(color[i], "specular color");
        // Preserve neutral-color reflectance while projecting RGB Fresnel onto Disney's scalar level and tint.
        result.principled.x *= 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
        result.principled.y = std::max({color[0], color[1], color[2]}) - std::min({color[0], color[1], color[2]});
    }
    if (source.has_sheen) {
        const auto *color = source.sheen.sheen_color_factor;
        for (unsigned i = 0; i < 3; ++i)
            Unit(color[i], "sheen color");
        result.sheen.x = std::max({color[0], color[1], color[2]});
        result.sheen.y =
            result.sheen.x > 0 ? (result.sheen.x - std::min({color[0], color[1], color[2]})) / result.sheen.x : 0;
        Unit(source.sheen.sheen_roughness_factor, "sheen roughness");
    }
    if (source.has_anisotropy) {
        result.principled.z = Unit(source.anisotropy.anisotropy_strength, "anisotropy");
        result.extras.x = Finite(source.anisotropy.anisotropy_rotation, "anisotropy rotation");
        Require(!source.anisotropy.anisotropy_texture.texture,
                "anisotropy direction textures are unsupported; use the material anisotropy strength and rotation");
    }
    // Keep authored coverage and sidedness separate from the hit-dependent front/back flag in flags.w.
    result.flags.x = (!source.has_volume || source.volume.thickness_factor == 0) ? 1u : 0u;
    result.flags.y = source.double_sided ? 1u : 0u;
    switch (source.alpha_mode) {
    case cgltf_alpha_mode_opaque:
        result.flags.z = 0;
        break;
    case cgltf_alpha_mode_mask:
        result.flags.z = 1;
        break;
    case cgltf_alpha_mode_blend:
        result.flags.z = 2;
        break;
    default:
        throw std::runtime_error("unsupported glTF alpha mode");
    }
    result.extras.z = Unit(source.alpha_cutoff, "alpha cutoff");
    result.extras.w = Unit(pbr.base_color_factor[3], "base alpha");
    result.extras.y = Positive(source.normal_texture.scale, "normal scale");
    // Volume data describes the existing homogeneous attenuation model, rather than unsupported thickness maps.
    if (source.has_volume) {
        Positive(source.volume.thickness_factor, "volume thickness");
        Require(!source.volume.thickness_texture.texture, "volume thickness textures are unsupported");
        result.medium = {Unit(source.volume.attenuation_color[0], "attenuation color"),
                         Unit(source.volume.attenuation_color[1], "attenuation color"),
                         Unit(source.volume.attenuation_color[2], "attenuation color"),
                         source.volume.attenuation_distance == std::numeric_limits<float>::max()
                             ? 0
                             : Positive(source.volume.attenuation_distance, "attenuation distance")};
    }
    if (source.has_diffuse_transmission) {
        result.sheen.w = Unit(source.diffuse_transmission.diffuse_transmission_factor, "diffuse transmission");
        Require(!source.diffuse_transmission.diffuse_transmission_texture.texture &&
                    !source.diffuse_transmission.diffuse_transmission_color_texture.texture,
                "diffuse transmission textures are unsupported");
        for (float channel : source.diffuse_transmission.diffuse_transmission_color_factor)
            Require(channel == 1, "colored diffuse transmission is unsupported");
    }
    // Base textures keep independent transforms; paired extension maps share a packed RG/RGBA runtime view.
    result.textures = {TextureView(context, pbr.base_color_texture),
                       TextureView(context, pbr.metallic_roughness_texture),
                       TextureView(context, source.normal_texture), TextureView(context, source.emissive_texture)};
    if (source.has_transmission)
        result.textures2.x = TextureView(context, source.transmission.transmission_texture);
    if (source.has_clearcoat) {
        Require(!source.clearcoat.clearcoat_normal_texture.texture,
                "independent clearcoat normal textures are unsupported");
        result.textures2.y = PackedView(context, source.clearcoat.clearcoat_texture,
                                        source.clearcoat.clearcoat_roughness_texture, 1, true);
    }
    if (source.has_sheen)
        result.textures2.z =
            PackedView(context, source.sheen.sheen_color_texture, source.sheen.sheen_roughness_texture, 3, false);
    if (source.has_specular)
        result.textures2.w =
            PackedView(context, source.specular.specular_color_texture, source.specular.specular_texture, 3, false);
    return result;
}
} // namespace
void ImportMaterials(ImportContext &context) {
    Require(context.data.materials_count < 0x01000000u, "material count exceeds exact float index storage");
    for (cgltf_size i = 0; i < context.data.materials_count; ++i) {
        context.scene.materials.push_back(Material(context, context.data.materials[i]));
        context.scene.materialNames.push_back(Name(context.data.materials[i].name, "Material " + std::to_string(i)));
    }
    // glTF allows primitives without a material; retain a dedicated standard default at the final index.
    GpuMaterial fallback;
    fallback.baseRoughness = {1, 1, 1, 1};
    fallback.emissionMetallic.w = 1;
    fallback.transmissionIor.y = 1.5f;
    fallback.flags.x = 1;
    context.scene.materials.push_back(fallback);
    context.scene.materialNames.emplace_back("Default Material");
}
} // namespace render::gltf
