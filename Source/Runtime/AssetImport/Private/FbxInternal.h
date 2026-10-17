#pragma once
#include "asset/FbxImporter.h"
#include "render/Math.h"
#include <array>
#include <map>
#include <set>
#include <string>
#include <ufbx.h>

namespace render::fbx {
struct ImportContext {
    const ufbx_scene &data;                                        // 官方解析器所有者覆盖完整转换过程。
    const std::filesystem::path &source;                           // 解析外部纹理时使用的原生绝对模型路径。
    Scene &scene;                                                  // 成功前只存在于当前事务中的完全拥有数据。
    std::map<std::uint32_t, Uint4> images;                         // 相同 FBX 图像文件仅解码一次。
    std::map<std::uint32_t, std::set<std::string>> materialUvSets; // 各材质实际使用的命名 UV 集。
    std::map<std::array<std::uint32_t, 8>, Uint4> packedImages;    // 相同输入像素与通道契约只生成一份合并图像。
};
void Require(bool condition, const std::string &message);
float Finite(double value, const char *description);
std::string Name(ufbx_string value, const std::string &fallback);
void ImportMaterials(ImportContext &context);
void ImportGeometry(ImportContext &context);
}
