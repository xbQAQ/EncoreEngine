#include "GltfInternal.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace render::gltf {
void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error("glTF import: " + message);
}
std::string Name(const char *source, const std::string &fallback) {
    return source && source[0] ? source : fallback;
}
float Finite(double value, const char *description) {
    Require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
            std::string(description) + " exceeds finite float range");
    return static_cast<float>(value);
}
namespace {
cgltf_result ReadFile(const cgltf_memory_options *, const cgltf_file_options *, const char *path, cgltf_size *size,
                      void **destination) {
    // 使用原生 Unicode 路径读取，避免解析器窄字符 fopen 破坏作者文件名。
    try {
        const auto bytes = ReadBytes(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path))));
        if (*size && bytes.size() < *size)
            return cgltf_result_data_too_short;
        *destination = std::malloc(bytes.size());
        if (!*destination)
            return cgltf_result_out_of_memory;
        std::memcpy(*destination, bytes.data(), bytes.size());
        *size = bytes.size();
        return cgltf_result_success;
    } catch (...) {
        return cgltf_result_file_not_found;
    }
}
void ReleaseFile(const cgltf_memory_options *, const cgltf_file_options *, void *data) {
    std::free(data);
}
const char *Result(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "truncated data";
    case cgltf_result_unknown_format:
        return "unknown format";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF data";
    case cgltf_result_invalid_options:
        return "invalid parser options";
    case cgltf_result_file_not_found:
        return "buffer file not found";
    case cgltf_result_io_error:
        return "file I/O error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.x is unsupported";
    default:
        return "parser error";
    }
}
} // 匿名命名空间
void ValidateFeatures(const cgltf_data &data) {
    // 在解码图像或发布目标数据前拒绝会丢失语义的静态转换。
    const std::set<std::string> supported = {"KHR_materials_clearcoat",
                                             "KHR_materials_transmission",
                                             "KHR_materials_ior",
                                             "KHR_materials_specular",
                                             "KHR_materials_sheen",
                                             "KHR_materials_anisotropy",
                                             "KHR_materials_volume",
                                             "KHR_materials_emissive_strength",
                                             "KHR_materials_diffuse_transmission",
                                             "KHR_texture_transform",
                                             "KHR_lights_punctual",
                                             "KHR_mesh_quantization"};
    for (cgltf_size i = 0; i < data.extensions_required_count; ++i)
        Require(supported.contains(data.extensions_required[i]),
                "unsupported required extension: " + std::string(data.extensions_required[i]));
    Require(data.animations_count == 0, "animated assets are unsupported; export a static Blender pose");
    Require(data.skins_count == 0, "skinned assets are unsupported; apply armature deformation before export");
    for (cgltf_size i = 0; i < data.buffer_views_count; ++i)
        Require(!data.buffer_views[i].has_meshopt_compression, "EXT_meshopt_compression is unsupported");
    for (cgltf_size i = 0; i < data.meshes_count; ++i) {
        const auto &mesh = data.meshes[i];
        Require(mesh.weights_count == 0, "mesh morph weights are unsupported");
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
            Require(!mesh.primitives[p].has_draco_mesh_compression,
                    "KHR_draco_mesh_compression is unsupported; disable export compression");
            Require(mesh.primitives[p].targets_count == 0,
                    "morph target geometry is unsupported; apply shape keys before export");
            Require(mesh.primitives[p].mappings_count == 0, "material variants are unsupported");
        }
    }
    // 世界变换计算会遍历父链，调用前先拒绝循环祖先关系。
    for (cgltf_size i = 0; i < data.nodes_count; ++i) {
        const auto &node = data.nodes[i];
        Require(!node.skin && node.weights_count == 0, "skinned or morphed nodes are unsupported");
        Require(!node.has_mesh_gpu_instancing,
                "EXT_mesh_gpu_instancing is unsupported; realize Blender instances before export");
        std::set<const cgltf_node *> ancestry;
        for (const cgltf_node *parent = &node; parent; parent = parent->parent)
            Require(ancestry.insert(parent).second, "node hierarchy contains a cycle");
    }
}
} // 命名空间 render::gltf
namespace render {
Scene ImportGltf(const std::filesystem::path &input) {
    // 输入字节与解析器所有者覆盖全部转换过程，包含 GLB 内嵌缓冲视图。
    const auto path = std::filesystem::absolute(input).lexically_normal();
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
    gltf::Require(extension == L".gltf" || extension == L".glb", "asset extension must be .gltf or .glb");
    const auto bytes = gltf::ReadBytes(path);
    cgltf_options options{};
    options.file.read = gltf::ReadFile;
    options.file.release = gltf::ReleaseFile;
    cgltf_data *parsed = nullptr;
    const auto parse = cgltf_parse(&options, bytes.data(), bytes.size(), &parsed);
    gltf::Require(parse == cgltf_result_success, "cannot parse asset: " + std::string(gltf::Result(parse)));
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owner(parsed, &cgltf_free);
    gltf::ValidateFeatures(*parsed);
    const auto encoded = path.u8string();
    const std::string utf8(encoded.begin(), encoded.end());
    const auto load = cgltf_load_buffers(&options, parsed, utf8.c_str());
    gltf::Require(load == cgltf_result_success, "cannot load buffers: " + std::string(gltf::Result(load)));
    const auto validate = cgltf_validate(parsed);
    gltf::Require(validate == cgltf_result_success, "asset validation failed: " + std::string(gltf::Result(validate)));
    // 场景只在局部事务成功后交付，解析器借用内存和半完成数据都不能逃逸。
    Scene result;
    result.sourcePath = utf8;
    gltf::ImportContext context{*parsed, path, result, {}};
    gltf::ImportMaterials(context);
    gltf::ImportGeometry(context);
    gltf::ImportLights(context);
    return result;
}
} // 命名空间 render
