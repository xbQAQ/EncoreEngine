#pragma once

#include "render/Scene.h"
#include "render/Math.h"
#include "World.pb.h"
#include <filesystem>
#include <string_view>

namespace render {
// 文档直接使用官方 protoc 生成的消息，文本与二进制共享同一套结构和校验。
using WorldDocument = config::World;
WorldDocument LoadWorldDocument(const std::filesystem::path& path);
void SaveWorldDocument(const WorldDocument& document, const std::filesystem::path& path);
void ValidateWorldDocument(const WorldDocument& document);
const config::Scene& FindDocumentScene(const WorldDocument& document, std::string_view sceneId);
Scene BakeScene(const WorldDocument& document, std::string_view sceneId, bool applyObjectTransforms = true);
std::filesystem::path DefaultWorldPath();
// 参数转换只在文档与渲染传输边界进行，纹理关联始终由对象引用提供。
GpuMaterial ReadMaterialParameters(const config::MaterialParameters& parameters);
void WriteMaterialParameters(const GpuMaterial& parameters, config::MaterialParameters& output);
Vec3 ReadVector(const config::Vec3& value);
void WriteVector(const Vec3& value, config::Vec3& output);
}
