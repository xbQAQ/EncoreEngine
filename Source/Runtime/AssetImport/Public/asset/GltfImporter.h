#pragma once

#include "render/Scene.h"
#include <filesystem>

namespace render {
// 将静态 glTF 2.0/GLB 转换为完全拥有的世界空间数据，无效或不支持的输入抛出异常。
Scene ImportGltf(const std::filesystem::path &path);
} // 命名空间 render
