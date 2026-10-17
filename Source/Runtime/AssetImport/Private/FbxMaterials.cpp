#include "FbxInternal.h"
#include "ImportImages.h"
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

namespace render::fbx {
namespace {
float Unit(double value, const char *label) {
    const auto result = Finite(value, label);
    Require(result >= 0 && result <= 1, std::string(label) + "必须在零到一之间");
    return result;
}
float Scalar(const ufbx_material_map &map, float fallback) {
    return map.has_value ? Finite(map.value_real, "材质标量") : fallback;
}
bool Active(const ufbx_material_map &map) { return map.texture && map.texture_enabled && !map.feature_disabled; }
std::filesystem::path TexturePath(const ImportContext &context, const ufbx_texture &texture) {
    // 优先消费可搬移的相对路径，再尝试来源绝对路径和模型同目录文件名。
    std::vector<std::filesystem::path> candidates;
    for (const auto name : {texture.relative_filename, texture.filename, texture.absolute_filename}) {
        if (!name.length)
            continue;
        const std::string value(name.data, name.length);
        Require(value.find('\0') == std::string::npos && value.find("://") == std::string::npos,
                "纹理路径包含空字符或远端地址");
        const auto path =
            std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
        candidates.push_back(path.is_absolute() ? path : context.source.parent_path() / path);
        candidates.push_back(context.source.parent_path() / path.filename());
    }
    for (const auto &candidate : candidates)
        if (std::filesystem::is_regular_file(candidate))
            return candidate;
    throw std::runtime_error("FBX 导入：外部纹理不存在：" +
                             Name(texture.relative_filename, Name(texture.filename, "未命名图像")));
}
GpuTextureView View(ImportContext &context, const ufbx_material_map &map, std::uint32_t material) {
    const auto &texture = *map.texture;
    Require(texture.type == UFBX_TEXTURE_FILE, "暂不支持分层、程序或着色器纹理，请先烘焙为 PNG/JPEG");
    if (texture.uv_set.length)
        context.materialUvSets[material].insert(Name(texture.uv_set, ""));
    const auto key = texture.has_file ? texture.file_index : texture.element_id | 0x80000000u;
    auto found = context.images.find(key);
    if (found == context.images.end()) {
        // 嵌入字节和本地外部图像进入同一解码器；缺图必须明确失败。
        Uint4 image;
        if (texture.content.size)
            image = asset_detail::DecodeImage(
                context.scene, {static_cast<const std::uint8_t *>(texture.content.data), texture.content.size});
        else
            image = asset_detail::DecodeImage(context.scene, asset_detail::ReadBytes(TexturePath(context, texture)));
        found = context.images.emplace(key, image).first;
    }
    GpuTextureView view;
    view.image = found->second;
    view.image.w = (texture.wrap_u == UFBX_WRAP_CLAMP ? 1u : 0u) | (texture.wrap_v == UFBX_WRAP_CLAMP ? 4u : 0u);
    if (texture.has_uv_transform) {
        // 顶点 V 已转为图像向下坐标，采样变换相应使用 F * M * F，避免上下翻转两次。
        const auto transform = [&](double x, double y) {
            const auto p = ufbx_transform_position(&texture.uv_to_texture, {x, 1 - y, 0});
            Require(std::abs(p.z) < 1e-6, "纹理变换不属于二维 UV 平面");
            return Vec3{Finite(p.x, "纹理变换"), Finite(1 - p.y, "纹理变换"), 0};
        };
        const auto origin = transform(0, 0), x = transform(1, 0) - origin, y = transform(0, 1) - origin;
        const float scaleX = Length(x);
        Require(scaleX > 0 && Length(y) > 0 && std::abs(Dot(x, y)) < 1e-5f * scaleX * Length(y),
                "不支持奇异或剪切 UV 变换");
        const float cosine = x.x / scaleX, sine = x.y / scaleX;
        const float scaleY = Dot(y, Vec3{-sine, cosine, 0});
        view.offsetScale = {origin.x, origin.y, scaleX, scaleY};
        view.rotation = {sine, cosine, 0, 0};
    }
    return view;
}
std::uint32_t AddView(ImportContext &context, const GpuTextureView &view) {
    Require(context.scene.textureViews.size() < std::numeric_limits<std::uint32_t>::max(),
            "纹理视图数量超过三十二位容量");
    const auto result = static_cast<std::uint32_t>(context.scene.textureViews.size());
    context.scene.textureViews.push_back(view);
    return result;
}
std::uint32_t Single(ImportContext &context, const ufbx_material_map &map, std::uint32_t material) {
    return Active(map) ? AddView(context, View(context, map, material)) : ~0u;
}
bool SameMapping(const GpuTextureView &a, const GpuTextureView &b) {
    return a.image.w == b.image.w && a.offsetScale.x == b.offsetScale.x && a.offsetScale.y == b.offsetScale.y &&
           a.offsetScale.z == b.offsetScale.z && a.offsetScale.w == b.offsetScale.w && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y;
}
std::uint32_t Pixel(const Scene &scene, const GpuTextureView &view, unsigned x, unsigned y, unsigned width,
                    unsigned height) {
    const auto px = std::min(view.image.y - 1, static_cast<unsigned>((std::uint64_t(x) * 2 + 1) * view.image.y /
                                                                     (std::uint64_t(width) * 2)));
    const auto py = std::min(view.image.z - 1, static_cast<unsigned>((std::uint64_t(y) * 2 + 1) * view.image.z /
                                                                     (std::uint64_t(height) * 2)));
    return scene.packedTexels[view.image.x + std::size_t(py) * view.image.y + px];
}
std::uint32_t Packed(ImportContext &context, const ufbx_material_map &first, const ufbx_material_map &second,
                     std::uint32_t material, bool baseOpacity, bool invertRoughness = false,
                     bool opacityAlpha = false) {
    if (!Active(first) && !Active(second))
        return ~0u;
    const auto a = Active(first) ? View(context, first, material) : GpuTextureView{};
    const auto b = Active(second) ? View(context, second, material) : GpuTextureView{};
    Require(!Active(first) || !Active(second) || SameMapping(a, b), "需要打包的纹理必须共享 UV 变换和包裹方式");
    auto combined = Active(first) ? a : b;
    // Blender 将同一基础色图像的 Alpha 再连到透明属性，已有 RGBA 无需重复生成或平方 Alpha。
    if (baseOpacity && opacityAlpha && Active(first) && Active(second) && a.image.x == b.image.x &&
        a.image.y == b.image.y && a.image.z == b.image.z)
        return AddView(context, a);
    const std::array<std::uint32_t, 8> key{a.image.x,
                                           a.image.y,
                                           a.image.z,
                                           b.image.x,
                                           b.image.y,
                                           b.image.z,
                                           baseOpacity ? (opacityAlpha ? 2u : 1u) : 0u,
                                           invertRoughness ? 1u : 0u};
    if (const auto found = context.packedImages.find(key); found != context.packedImages.end()) {
        const auto wrap = combined.image.w;
        combined.image = found->second;
        combined.image.w = wrap;
        return AddView(context, combined);
    }
    const auto width = std::max(a.image.y, b.image.y), height = std::max(a.image.z, b.image.z);
    Require(context.scene.packedTexels.size() + std::uint64_t(width) * height <=
                std::numeric_limits<std::uint32_t>::max(),
            "打包图像超过偏移容量");
    const auto offset = static_cast<std::uint32_t>(context.scene.packedTexels.size());
    // FBX 独立标量图转换为现有基础色 RGBA 或金属粗糙度 G/B 通道契约。
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto firstPixel = Active(first) ? Pixel(context.scene, a, x, y, width, height) : 0xffffffffu;
            const auto secondPixel = Active(second) ? Pixel(context.scene, b, x, y, width, height) : 0xffffffffu;
            if (baseOpacity) {
                const auto opacity = opacityAlpha ? secondPixel >> 24 : secondPixel & 255u;
                context.scene.packedTexels.push_back((firstPixel & 0x00ffffffu) |
                                                     ((((firstPixel >> 24) * opacity + 127u) / 255u) << 24));
            } else {
                const auto rough = invertRoughness ? 255u - (firstPixel & 255u) : firstPixel & 255u;
                context.scene.packedTexels.push_back(255u | (rough << 8) | ((secondPixel & 255u) << 16) | 0xff000000u);
            }
        }
    combined.image = {offset, width, height, combined.image.w};
    context.packedImages.emplace(key, combined.image);
    return AddView(context, combined);
}
GpuMaterial Material(ImportContext &context, const ufbx_material &source) {
    const auto &p = source.pbr;
    const auto id = source.typed_id;
    const auto &base = p.base_color.has_value || Active(p.base_color) ? p.base_color : source.fbx.diffuse_color;
    const auto &emission =
        p.emission_color.has_value || Active(p.emission_color) ? p.emission_color : source.fbx.emission_color;
    const auto color = base.has_value ? base.value_vec3 : ufbx_vec3{.8, .8, .8};
    const float baseFactor =
        Unit(p.base_color.has_value ? Scalar(p.base_factor, 1) : Scalar(source.fbx.diffuse_factor, 1), "基础色倍率");
    GpuMaterial result;
    // 材质常数按线性空间消费，PNG/JPEG 原始颜色字节只在对应着色器槽进行 sRGB 解码。
    result.baseRoughness = {Unit(color.x * baseFactor, "基础色"), Unit(color.y * baseFactor, "基础色"),
                            Unit(color.z * baseFactor, "基础色"), .5f};
    const bool glossy = !Active(p.roughness) && Active(p.glossiness);
    const auto &roughness = glossy ? p.glossiness : p.roughness;
    const float phongRoughness =
        source.fbx.specular_exponent.has_value
            ? Finite(std::sqrt(2.0 / (std::max(source.fbx.specular_exponent.value_real, 0.0) + 2)), "Phong 粗糙度")
            : .5f;
    result.baseRoughness.w = Unit(Scalar(p.roughness, Active(roughness) ? 1 : phongRoughness), "粗糙度");
    result.emissionMetallic.w = Unit(Scalar(p.metalness, Active(p.metalness) ? 1.0f : 0.0f), "金属度");
    const float emissionFactor = Scalar(p.emission_color.has_value ? p.emission_factor : source.fbx.emission_factor,
                                        emission.has_value ? 1.0f : 0.0f);
    Require(emissionFactor >= 0, "自发光倍率不能为负数");
    result.emissionMetallic.x = Finite(emission.value_vec3.x * emissionFactor, "自发光");
    result.emissionMetallic.y = Finite(emission.value_vec3.y * emissionFactor, "自发光");
    result.emissionMetallic.z = Finite(emission.value_vec3.z * emissionFactor, "自发光");
    result.transmissionIor = {Unit(Scalar(p.transmission_factor, 0), "透射"), Scalar(p.specular_ior, 1.5f),
                              Unit(Scalar(p.coat_factor, 0), "清漆"),
                              1 - Unit(Scalar(p.coat_roughness, 0), "清漆粗糙度")};
    Require(result.transmissionIor.y >= 1 && result.transmissionIor.y <= 4, "折射率必须在一到四之间");
    result.principled.x = Unit(Scalar(p.specular_factor, Scalar(source.fbx.specular_factor, .5f)), "高光强度");
    result.principled.z = Unit(Scalar(p.specular_anisotropy, 0), "各向异性");
    result.extras.x = Scalar(p.specular_rotation, 0);
    result.sheen.x = Unit(Scalar(p.sheen_factor, 0), "绒毛");
    result.flags.x = source.features.thin_walled.enabled ? 1u : 0u;
    result.flags.y = source.features.double_sided.enabled ? 1u : 0u;
    result.extras.w = Unit(Scalar(p.opacity, 1), "覆盖率");
    Require(result.extras.w == 1 || result.extras.w == 0, "不支持部分透明覆盖混合，请使用物理透射或二值裁剪纹理");
    result.flags.z = Active(p.opacity) || result.extras.w == 0 ? 1u : 0u;
    Require(!source.features.unlit.enabled, "暂不支持无光照材质");
    // 颜色与覆盖纹理可独立输入，合并后仍只占现有基础色槽。
    result.textures.x = Active(p.opacity) ? Packed(context, base, p.opacity, id, true, false,
                                                   source.shader_type == UFBX_SHADER_BLENDER_PHONG)
                                          : Single(context, base, id);
    result.textures.y = Packed(context, roughness, p.metalness, id, false, glossy);
    const auto &normal = Active(p.normal_map) ? p.normal_map : source.fbx.normal_map;
    result.textures.z = Single(context, normal, id);
    result.textures.w = Single(context, emission, id);
    result.textures2.x = Single(context, p.transmission_factor, id);
    // 不能表达的贴图不应静默消失；标量高级参数仍保留在现有 Disney 通道。
    for (std::size_t i = 0; i < UFBX_MATERIAL_PBR_MAP_COUNT; ++i) {
        if (i == UFBX_MATERIAL_PBR_BASE_COLOR || i == UFBX_MATERIAL_PBR_OPACITY || i == UFBX_MATERIAL_PBR_ROUGHNESS ||
            i == UFBX_MATERIAL_PBR_GLOSSINESS || i == UFBX_MATERIAL_PBR_METALNESS ||
            i == UFBX_MATERIAL_PBR_NORMAL_MAP || i == UFBX_MATERIAL_PBR_EMISSION_COLOR ||
            i == UFBX_MATERIAL_PBR_TRANSMISSION_FACTOR)
            continue;
        Require(!Active(p.maps[i]), "当前材质参数的独立纹理无法映射到运行时槽，编号：" + std::to_string(i));
    }
    Require(!Active(source.fbx.bump) && !Active(source.fbx.displacement), "请先将高度凹凸或位移烘焙为切线空间法线");
    return result;
}
}
void ImportMaterials(ImportContext &context) {
    Require(context.data.materials.count < 0x00ffffffu, "材质数量超过精确浮点索引容量");
    for (const auto *source : context.data.materials) {
        Require(source->typed_id == context.scene.materials.size(), "官方材质标识顺序不连续");
        context.scene.materials.push_back(Material(context, *source));
        context.scene.materialNames.push_back(Name(source->name, "FBX 材质 " + std::to_string(source->typed_id)));
    }
    // 无材质表面拥有独立中性默认槽，不与任意作者材质误关联。
    GpuMaterial fallback;
    fallback.baseRoughness = {.8f, .8f, .8f, .5f};
    fallback.transmissionIor.y = 1.5f;
    context.scene.materials.push_back(fallback);
    context.scene.materialNames.emplace_back("FBX 默认材质");
}
}
