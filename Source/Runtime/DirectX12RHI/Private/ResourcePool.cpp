#include "dx12/ResourcePool.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/PassParameters.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace render::dx12 {
ResourcePool::ResourcePool(Dx12Device& device, DescriptorArena& descriptors) : device_(device), descriptors_(descriptors) {}

GpuResource& ResourcePool::AcquireTransient(const ResourceDescriptor& description, const std::string& name) {
    // 空闲信号不能退休尚未提交的录制；完成值还必须受最近实际命令提交的 fence 限制。
    const auto retired = std::min(device_.CompletedFenceValue(), device_.LastSubmissionFenceValue());
    const auto nextSubmission = device_.SubmittedFenceValue() + 1;
    for (auto& entry : transient_) {
        if (entry.description == description && entry.retireFence <= retired) {
            entry.retireFence = nextSubmission;
            return *entry.resource;
        }
    }
    if (description.kind == ResourceKind::Buffer)
        Buffer(name, description.width, description.stride, static_cast<D3D12_RESOURCE_FLAGS>(description.flags),
            description.memory == ResourceMemory::Upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
            description.memory == ResourceMemory::Upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
    else
        Texture(name, static_cast<unsigned>(description.width), description.height, static_cast<DXGI_FORMAT>(description.format),
            static_cast<D3D12_RESOURCE_FLAGS>(description.flags), description.clear);
    // 将物理存储缓存与下一帧需要导入的长期资源分开。
    auto allocation = std::move(resources_.back());
    resources_.pop_back();
    transient_.push_back({description, std::move(allocation), nextSubmission});
    return *transient_.back().resource;
}

void ResourcePool::ValidateExtraction(const ResourceDescriptor& description, const GpuResource& destination) const {
    // 历史轮换只接受默认堆；上传和读回的 CPU 可访问性不能随原生对象交换而改变。
    const bool external = std::any_of(resources_.begin(), resources_.end(), [&](const auto& entry) { return entry.get() == &destination; });
    if (!external || !destination.object || description.memory != ResourceMemory::Default)
        throw std::invalid_argument("RDG extraction requires a pooled default-heap destination and default-heap source");
    D3D12_HEAP_PROPERTIES heap{};
    // 驱动返回的默认堆可带附加标志；内存类型决定能否与池内默认堆交换。
    Check(destination.object->GetHeapProperties(&heap, nullptr), "Query extraction destination heap");
    if (heap.Type != D3D12_HEAP_TYPE_DEFAULT)
        throw std::invalid_argument("RDG extraction cannot exchange upload or readback heaps");
    const auto target = destination.object->GetDesc();
    const bool buffer = description.kind == ResourceKind::Buffer;
    // 创建时的零对齐由 D3D12 规范化为 64 KiB；两种表示对应相同的非 MSAA 默认分配要求。
    const bool defaultAlignment = target.Alignment == 0 || target.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    if (target.Dimension != (buffer ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D) ||
        !defaultAlignment || target.Width != (buffer ? std::max<std::uint64_t>(description.width, 256) : description.width) ||
        target.Height != description.height || target.DepthOrArraySize != 1 || target.MipLevels != 1 ||
        target.Format != static_cast<DXGI_FORMAT>(description.format) || target.SampleDesc.Count != 1 || target.SampleDesc.Quality != 0 ||
        target.Layout != (buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN) ||
        static_cast<unsigned>(target.Flags) != description.flags || destination.stride != description.stride ||
        (buffer && description.stride && destination.count != description.width / description.stride)) {
        // 输出实际原生描述，区分驱动规范化字段与真正不兼容的历史分配。
        std::ostringstream message;
        message << "RDG extraction allocations have incompatible descriptors: " << destination.name
            << "; native dimension/alignment/width/height/depth/mips/format/samples/quality/layout/flags/stride/count="
            << target.Dimension << '/' << target.Alignment << '/' << target.Width << '/' << target.Height << '/'
            << target.DepthOrArraySize << '/' << target.MipLevels << '/' << target.Format << '/'
            << target.SampleDesc.Count << '/' << target.SampleDesc.Quality << '/' << target.Layout << '/'
            << target.Flags << '/' << destination.stride << '/' << destination.count
            << "; graph kind/width/height/format/flags/stride=" << static_cast<unsigned>(description.kind) << '/'
            << description.width << '/' << description.height << '/' << description.format << '/' << description.flags << '/' << description.stride;
        throw std::invalid_argument(message.str());
    }
}

void ResourcePool::ExchangeTransient(GpuResource& physical, GpuResource& destination) {
    const auto cached = std::find_if(transient_.begin(), transient_.end(), [&](const auto& entry) { return entry.resource.get() == &physical; });
    if (cached == transient_.end()) throw std::invalid_argument("RDG extraction source is not pooled transient storage");
    ValidateExtraction(cached->description, destination);
    // 保留双方逻辑身份，仅交换原生所有权；旧历史仍被缓存持有直到本次提交完成。
    using std::swap;
    swap(physical.object, destination.object);
    swap(physical.state, destination.state);
    swap(physical.srv, destination.srv);
    swap(physical.uav, destination.uav);
    swap(physical.clearUav, destination.clearUav);
    swap(physical.stride, destination.stride);
    swap(physical.count, destination.count);
}

GpuResource& ResourcePool::Buffer(const std::string& name, UINT64 bytes, unsigned stride,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, D3D12_HEAP_TYPE heap) {
    auto value = std::make_unique<GpuResource>();
    value->name = name;
    value->state = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : state;
    // 普通默认堆缓冲区从 COMMON 开始；加速结构使用不可变的专用状态。
    if (heap == D3D12_HEAP_TYPE_DEFAULT && value->state == Uav) value->state = D3D12_RESOURCE_STATE_COMMON;
    value->stride = stride;
    value->count = stride ? static_cast<unsigned>(bytes / stride) : 0;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = std::max<UINT64>(bytes, 256);
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    Check(device_.Native()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
        value->state, nullptr, IID_PPV_ARGS(&value->object)), "Buffer allocation");
    value->object->SetName(std::wstring(name.begin(), name.end()).c_str());
    auto& resource = *resources_.emplace_back(std::move(value));
    if (stride) MakeViews(resource);
    return resource;
}

GpuResource& ResourcePool::Texture(const std::string& name, unsigned width, unsigned height,
    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, std::array<float, 4> renderClear) {
    auto value = std::make_unique<GpuResource>();
    value->name = name;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Format = format;
    description.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    // 匹配实际 RTV 清除值，避免调试层警告并保留快速清除元数据。
    std::copy(renderClear.begin(), renderClear.end(), clear.Color);
    if (format == DXGI_FORMAT_D32_FLOAT) clear.DepthStencil.Depth = 1;
    const bool optimized = (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    Check(device_.Native()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
        value->state, optimized ? &clear : nullptr, IID_PPV_ARGS(&value->object)), "Texture allocation");
    value->object->SetName(std::wstring(name.begin(), name.end()).c_str());
    auto& resource = *resources_.emplace_back(std::move(value));
    if (format != DXGI_FORMAT_D32_FLOAT) MakeViews(resource);
    return resource;
}

void ResourcePool::MakeViews(GpuResource& resource) {
    const auto description = resource.object->GetDesc();
    resource.srv = descriptors_.AllocatePersistent();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (resource.stride) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = resource.count;
        srv.Buffer.StructureByteStride = resource.stride;
    } else {
        srv.Format = description.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
    }
    device_.Native()->CreateShaderResourceView(resource.object.Get(), &srv, descriptors_.Cpu(resource.srv));
    if ((description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) return;
    resource.uav = descriptors_.AllocatePersistent();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    if (resource.stride) {
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = resource.count;
        uav.Buffer.StructureByteStride = resource.stride;
    } else {
        uav.Format = description.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    device_.Native()->CreateUnorderedAccessView(resource.object.Get(), nullptr, &uav, descriptors_.Cpu(resource.uav));
    resource.clearUav = resource.uav;
    if (resource.stride) {
        // 结构化 UAV 需要等价原始视图才能执行 ClearUnorderedAccessView。
        resource.clearUav = descriptors_.AllocatePersistent();
        D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
        raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.Buffer.NumElements = static_cast<UINT>(description.Width / 4);
        raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_.Native()->CreateUnorderedAccessView(resource.object.Get(), nullptr, &raw, descriptors_.Cpu(resource.clearUav));
    }
    descriptors_.CopyToVisible(resource.clearUav, resource.clearUav);
}

void ResourcePool::Upload(RenderGraph& graph, GraphExecutor& executor, GpuResource& destination, const void* source, size_t bytes) {
    if (!source || !bytes) throw std::invalid_argument("Upload requires nonempty source bytes");
    if (bytes > destination.object->GetDesc().Width) throw std::out_of_range("Upload exceeds destination capacity");
    // 每次上传拥有独立快照，不能按目标缓存同一份 CPU 可写暂存字节。
    auto& staging = executor.UploadParameters(graph, destination.name + ".Data", {static_cast<const std::byte*>(source), bytes});
    PassParameters parameters;
    parameters.Access(staging, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
    parameters.Access(destination, bytes == destination.object->GetDesc().Width ? ResourceAccess::Write : ResourceAccess::ReadWrite,
        D3D12_RESOURCE_STATE_COPY_DEST);
    parameters.AddPass(graph, destination.name + ".Upload",
        [this, &destination, &staging, bytes](const PassParameters&) {
            device_.Commands()->CopyBufferRegion(destination.object.Get(), 0, staging.object.Get(), 0, bytes);
        });
}

void ResourcePool::ClearWritable(RenderGraph& graph) {
    // 初始化是图可见的生产者，也覆盖首帧读取的历史资源。
    for (const auto& resource : resources_) {
        auto* target = resource.get();
        if (!target->uav) continue;
        graph.AddPass("Clear." + target->name, {Write(*target)}, [this, target] {
            const UINT zero[4]{};
            device_.Commands()->ClearUnorderedAccessViewUint(descriptors_.Gpu(target->clearUav), descriptors_.Cpu(target->clearUav),
                target->object.Get(), zero, 0, nullptr);
        });
        graph.Export(target->graphId, ShaderRead);
    }
}
}
