#pragma once

#include "asset/GltfImporter.h"
#include "render/Math.h"
#include <cgltf.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace render::gltf {
struct ImportContext {
    cgltf_data &data;                                   // 解析器所有者覆盖完整导入事务，转换过程中不会出现悬空引用。
    const std::filesystem::path &source;                // 解析外部图像 URI 时使用的原生绝对模型路径。
    Scene &scene;                                       // 只有完整导入成功后才发布的完全拥有目标数据。
    std::map<const cgltf_image *, Uint4> decodedImages; // 同一来源只解码一次，独立视图仍保留各自的 UV 变换。
};

// 解码、材质映射和世界空间几何转换保持独立职责，并共用同一个事务所有者。
std::vector<std::uint8_t> ReadBytes(const std::filesystem::path &path);
Uint4 DecodeImage(ImportContext &context, const cgltf_image &image);
std::uint32_t TextureView(ImportContext &context, const cgltf_texture_view &view);
void ImportMaterials(ImportContext &context);
void ImportGeometry(ImportContext &context);
void ImportLights(ImportContext &context);
void ValidateFeatures(const cgltf_data &data);
void Require(bool condition, const std::string &message);
std::string Name(const char *source, const std::string &fallback);
float Finite(double value, const char *description);
} // 命名空间 render::gltf
