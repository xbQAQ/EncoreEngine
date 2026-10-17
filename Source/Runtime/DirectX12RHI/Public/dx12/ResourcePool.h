#pragma once
#include "dx12/Dx12Common.h"
#include <memory>
#include <array>
#include <vector>

namespace render::dx12 {
class Dx12Device;
class DescriptorArena;
class GraphExecutor;

// 拥有物理分配和视图；队列提交与 Pass 调度由其他模块负责。
class ResourcePool {
public:
    ResourcePool(Dx12Device& device, DescriptorArena& descriptors);
    GpuResource& Buffer(const std::string& name, UINT64 bytes, unsigned stride = 0,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
        D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT);
    GpuResource& Texture(const std::string& name, unsigned width, unsigned height,
        DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, std::array<float, 4> renderClear = {});
    void Upload(RenderGraph& graph, GraphExecutor& executor, GpuResource& destination, const void* source, size_t bytes);
    void ClearWritable(RenderGraph& graph);
    // 只有上一张图的 GPU 提交完成后才能复用对应分配。
    GpuResource& AcquireTransient(const ResourceDescriptor& description, const std::string& name);
    // 建图和发布前共同验证提取边界，防止部分交换或不同堆类型污染缓存。
    void ValidateExtraction(const ResourceDescriptor& description, const GpuResource& destination) const;
    // 提取后的分配交给历史，旧历史进入同一 fence 保护的物理缓存。
    void ExchangeTransient(GpuResource& physical, GpuResource& destination);
    std::size_t TransientAllocationCount() const { return transient_.size(); }
    // 替换场景之前，调用方必须销毁资源使用者并等待 GPU 完成。
    void Reset() { transient_.clear(); resources_.clear(); }
    const std::vector<std::unique_ptr<GpuResource>>& All() const { return resources_; }
private:
    struct TransientAllocation {
        ResourceDescriptor description; // 精确的物理分配兼容要求。
        std::unique_ptr<GpuResource> resource; // 原生物理分配及其可复用描述符。
        std::uint64_t retireFence{}; // 另一张图复用之前必须完成的提交 fence 值。
    };
    void MakeViews(GpuResource& resource);
    Dx12Device& device_; // 由运行时拥有的设备分配接口。
    DescriptorArena& descriptors_; // 由运行时拥有的长期视图分配器。
    std::vector<std::unique_ptr<GpuResource>> resources_; // 指向地址稳定的物理分配。
    std::vector<TransientAllocation> transient_; // 完整资源缓存，不参与自动外部资源导入。
};
}
