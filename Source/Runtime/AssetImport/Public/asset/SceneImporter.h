#pragma once
#include "render/Scene.h"
#include <filesystem>

namespace render {
// 按格式导入为一个可整体编辑的静态模型，各三角形保留原始材质，结果完全拥有资源数据。
Scene ImportSceneFile(const std::filesystem::path &path);
}
