// 附件能力检查随资源类型维护，避免调用方各自校验。
#include "Resources/RenderTarget.h"
#include <stdexcept>
#include <utility>

namespace render {
RenderTarget::RenderTarget(std::string name, const ResourceDescriptor& description)
    : GraphTexture(std::move(name), description) {
    // 纯 UAV 计算纹理不能冒充光栅附件；RTV 与 DSV 能力也不能同时出现。
    const auto capabilities = description.flags &
        (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if (capabilities != D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && capabilities != D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        throw std::invalid_argument("RenderTarget requires exactly one render target or depth stencil capability");
}
}
