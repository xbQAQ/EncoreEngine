#pragma once
#include "dx12/Dx12Common.h"
#include <array>
#include <cstddef>
#include <vector>

namespace render::dx12 { class Dx12Device; class ResourcePool; class GraphExecutor; }
namespace render {
// 面光源拟合数据独立于场景资产，由初始化图显式上传到长期只读纹理。
class LtcLuts {
public:
    explicit LtcLuts(dx12::ResourcePool&);
    void Upload(RenderGraph&, dx12::GraphExecutor&, dx12::Dx12Device&);
    dx12::GpuResource& Matrix() const { return *textures_[0]; }
    dx12::GpuResource& Amplitude() const { return *textures_[1]; }
private:
    std::array<dx12::GpuResource*, 2> textures_{}; // RGBA16F 逆矩阵与振幅/Fresnel 积分表。
    std::array<std::vector<std::byte>, 2> pixels_; // 保持上游半精度字节，初始化提交失败后仍允许重录。
};
}
