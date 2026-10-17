#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace render {
struct MaterialShaderSource {
    std::uint32_t materialIndex{}; // 接收表面脚本的现有 GPU 材质槽。
    std::string source; // 定义 Surface(inout Material, float2 uv, float4 tangent) 的 HLSL 源码。
    std::string path; // UTF-8 源文件路径，独立于运行时着色器头文件查找目录。
    bool operator==(const MaterialShaderSource&) const = default;
};
struct MaterialCompileResult {
    bool success{}; // 所有受影响的着色器和原生管线是否已整体替换成功。
    std::string diagnostics; // UTF-8 中文提示及 DXC 或 D3D12 原始诊断。
};
inline std::string_view DefaultMaterialScript() {
    // 默认模板保持导入纹理的采样结果，直到作者主动修改光学因子。
    return "// 在纹理采样后修改光学因子；资源绑定、透明度和法线贴图保留作者配置。\nvoid Surface(inout Material material, float2 uv, float4 tangent)\n{\n    material.baseRoughness.xyz *= float3(1.0, 1.0, 1.0);\n}\n";
}
}
