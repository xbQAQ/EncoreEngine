#include "FbxInternal.h"
#include "ImportImages.h"
#include <algorithm>
#include <cwctype>
#include <limits>
#include <memory>
#include <stdexcept>

namespace render::fbx {
void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error("FBX 导入：" + message);
}
float Finite(double value, const char *description) {
    Require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
            std::string(description) + "超出有限浮点范围");
    return static_cast<float>(value);
}
std::string Name(ufbx_string value, const std::string &fallback) {
    return value.length ? std::string(value.data, value.length) : fallback;
}
}
namespace render {
Scene ImportFbx(const std::filesystem::path &input) {
    // 先通过原生 Unicode 文件系统读取，解析器无需使用窄字符 fopen 打开主文件。
    const auto path = std::filesystem::absolute(input).lexically_normal();
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
    fbx::Require(extension == L".fbx", "文件扩展名必须为 .fbx");
    const auto bytes = asset_detail::ReadBytes(path);
    const auto encoded = path.u8string();
    const std::string utf8(encoded.begin(), encoded.end());
    // 统一到米制、右手 Y 向上空间，几何专用变换仍由 geometry_to_world 完整消费。
    ufbx_load_opts options{};
    options.filename = {utf8.c_str(), utf8.size()};
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1;
    options.generate_missing_normals = true;
    options.evaluate_skinning = true;
    options.use_blender_pbr_material = true;
    options.strict = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit = 1024;
    options.temp_allocator.memory_limit = std::size_t{1} << 30;
    options.result_allocator.memory_limit = std::size_t{1} << 30;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> source(
        ufbx_load_memory(bytes.data(), bytes.size(), &options, &error), &ufbx_free_scene);
    if (!source) {
        char diagnostic[2048]{};
        ufbx_format_error(diagnostic, sizeof(diagnostic), &error);
        throw std::runtime_error(std::string("FBX 解析失败：") + diagnostic);
    }
    // 官方求值器烘焙文件默认姿态与当前形态权重，运行时不持有骨骼或形态键系统。
    fbx::Require(source->anim_curves.count == 0, "不支持动画曲线，请先导出静态姿态");
    fbx::Require(source->cache_deformers.count == 0, "不支持外部顶点缓存，请先应用缓存变形");
    Scene result;
    result.sourcePath = utf8;
    fbx::ImportContext context{*source, path, result, {}, {}, {}};
    fbx::ImportMaterials(context);
    fbx::ImportGeometry(context);
    return result;
}
}
