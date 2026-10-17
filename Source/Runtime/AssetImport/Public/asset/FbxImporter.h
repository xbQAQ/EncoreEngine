#pragma once
#include "render/Scene.h"
#include <filesystem>

namespace render {
// 使用固定版本 ufbx 解析静态 FBX；不支持的数据必须在发布场景前明确拒绝。
Scene ImportFbx(const std::filesystem::path &path);
}
