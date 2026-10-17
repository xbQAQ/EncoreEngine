#pragma once
#include "render/Scene.h"
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace render::asset_detail {
// 文件与图像共用原生 Unicode 路径和 CPU 解码，导入阶段不依赖 GPU 设备。
std::vector<std::uint8_t> ReadBytes(const std::filesystem::path &path);
Uint4 DecodeImage(Scene &scene, std::span<const std::uint8_t> bytes);
}
