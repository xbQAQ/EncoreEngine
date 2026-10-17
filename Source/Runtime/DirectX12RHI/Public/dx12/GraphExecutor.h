#pragma once
#include "dx12/Dx12Common.h"
#include <vector>
#include <deque>
#include <span>
#include <cstddef>

namespace render::dx12 {
class Dx12Device;
class ResourcePool;
class GraphProfiler;
inline Use Read(const GpuResource& resource, D3D12_RESOURCE_STATES state = ShaderRead) {
    return {resource.graphId, static_cast<unsigned>(state), ResourceAccess::Read};
}
inline Use Write(const GpuResource& resource, D3D12_RESOURCE_STATES state = Uav) {
    return {resource.graphId, static_cast<unsigned>(state), ResourceAccess::Write};
}
// 原地累积或保留未覆盖区域的写入必须读取旧值，不能声明为完整覆盖。
inline Use ReadWrite(const GpuResource& resource, D3D12_RESOURCE_STATES state = Uav) {
    return {resource.graphId, static_cast<unsigned>(state), ResourceAccess::ReadWrite};
}

// 物化由图拥有的资源，并翻译平台无关的执行计划。
class GraphExecutor {
public:
    explicit GraphExecutor(Dx12Device& device, GraphProfiler* profiler = nullptr) : device_(device), profiler_(profiler) {}
    void Import(RenderGraph& graph, ResourcePool& resources);
    void Import(RenderGraph& graph, GpuResource& resource);
    void Create(RenderGraph& graph, GpuResource& resource, const std::string& name, const ResourceDescriptor& description);
    GpuResource& UploadParameters(RenderGraph& graph, const std::string& passName, std::span<const std::byte> bytes);
    void Extract(RenderGraph& graph, const char* name, GpuResource& source, GpuResource& destination,
        D3D12_RESOURCE_STATES finalState = ShaderRead);
    void QueueExtraction(RenderGraph& graph, const char* name, GpuResource& source, GpuResource& destination,
        D3D12_RESOURCE_STATES finalState = ShaderRead);
    void Execute(RenderGraph& graph);
    const std::vector<std::string>& PassNames() const { return passNames_; }
private:
    struct Extraction {
        ResourceId source{}; // 延长至图执行结束的瞬时源资源。
        GpuResource* destination{}; // 执行成功后接收原生分配的稳定外部代理。
    };
    Dx12Device& device_; // 接收屏障及 Pass 回调的命令列表所属设备。
    GraphProfiler* profiler_{}; // 由帧渲染器拥有的可选长期时间戳收集器。
    ResourcePool* pool_{}; // 由帧外部导入边界指定的瞬时物理缓存。
    std::vector<GpuResource*> resources_; // 当前图的逻辑编号到原生分配映射。
    std::vector<std::string> passNames_; // 供编辑器检查的保留编译执行顺序。
    std::vector<Extraction> extractions_; // 图完成后统一发布的零拷贝历史轮换。
    std::deque<GpuResource> parameterResources_; // 地址稳定、寿命由图管理的逐 Pass 常量代理。
};
}
