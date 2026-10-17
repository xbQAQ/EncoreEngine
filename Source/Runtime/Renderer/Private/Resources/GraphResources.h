#pragma once
// 资源代理仅依赖 RHI 类型，不反向依赖帧调度或具体算法。
#include "dx12/Dx12Common.h"
#include <string>

namespace render::dx12 { class GraphExecutor; }
namespace render {
// 单张图内纹理只持有描述与稳定逻辑代理，物理存储由 RDG 编译寿命决定。
class GraphTexture {
public:
    GraphTexture(std::string name, const ResourceDescriptor& description);
    GraphTexture(const GraphTexture&) = delete;
    GraphTexture& operator=(const GraphTexture&) = delete;
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    dx12::GpuResource& Resource() { return resource_; }
    const dx12::GpuResource& Resource() const { return resource_; }
    dx12::GpuResource* Pointer() { return &resource_; }
    const ResourceDescriptor& Description() const { return description_; }
private:
    dx12::GpuResource resource_; // 当前图中地址稳定的纹理逻辑代理，声明阶段不分配原生对象。
    ResourceDescriptor description_; // 仅允许二维纹理的格式、尺寸、能力与清除值。
};

// 缓冲区使用独立类型，避免 reservoir 的字节数和结构步长混入纹理尺寸语义。
class GraphBuffer {
public:
    GraphBuffer(std::string name, const ResourceDescriptor& description);
    GraphBuffer(const GraphBuffer&) = delete;
    GraphBuffer& operator=(const GraphBuffer&) = delete;
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    dx12::GpuResource& Resource() { return resource_; }
    const dx12::GpuResource& Resource() const { return resource_; }
    dx12::GpuResource* Pointer() { return &resource_; }
    const ResourceDescriptor& Description() const { return description_; }
private:
    dx12::GpuResource resource_; // 当前图中地址稳定的缓冲区逻辑代理。
    ResourceDescriptor description_; // 仅允许线性缓冲区的字节数、结构步长和能力。
};
}
