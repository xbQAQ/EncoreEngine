#pragma once
#include "render/RenderGraph.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace render::dx12 {
using Microsoft::WRL::ComPtr;
inline constexpr auto ShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
inline constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// 在平台边界保留失败操作名称，便于定位原生 API 错误。
inline void Check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream text;
    text << operation << " failed (0x" << std::hex << static_cast<unsigned>(result) << ')';
    throw std::runtime_error(text.str());
}

struct GpuResource {
    ComPtr<ID3D12Resource> object; // 由物理池或交换链拥有的原生分配。
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON}; // RDG 最近建立的原生状态。
    std::string name; // 稳定的图与 GPU 调试标签。
    unsigned srv{}; // 着色器只读视图的长期描述符源。
    unsigned uav{}; // 着色器写入视图的长期描述符源。
    unsigned clearUav{}; // 初始化清除使用的等价类型或原始视图。
    unsigned stride{}; // 结构化元素步长；零表示纹理或原始缓冲区。
    unsigned count{}; // 结构化缓冲区元素数。
    ResourceId graphId{InvalidGraphIndex}; // 仅在所属图内有效的逻辑编号。
    const RenderGraph* graphOwner{}; // 防止其他图中数值相同的编号被误用于当前参数。
};
}
