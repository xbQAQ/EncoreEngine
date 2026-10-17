#pragma once
// 材质编译结果不包含 PSO 所有权，供管线缓存按需消费。
#include "render/MaterialScripts.h"
#include <cstddef>
#include <map>
#include <span>
#include <vector>

namespace render {
struct MaterialShaderBundle {
    std::map<std::wstring, std::vector<std::byte>> programs; // 候选 DXIL 程序，键名与管线缓存完全一致。
    MaterialCompileResult result; // 整组编译结果及诊断，不允许发布不完整的程序集合。
};
// 运行时编译使用构建所用的 DXC 及原始着色器包含目录。
MaterialShaderBundle CompileMaterialShaders(std::span<const MaterialShaderSource> sources, bool dxrSupported);
}
