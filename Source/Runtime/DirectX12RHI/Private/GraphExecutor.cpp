#include "dx12/GraphExecutor.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphProfiler.h"
#include "dx12/ResourcePool.h"
#include "dx12/PassParameters.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace render::dx12 {
void GraphExecutor::Import(RenderGraph& graph, ResourcePool& resources) {
    pool_ = &resources;
    resources_.clear();
    // 新图不能继承上一图的逻辑代理和延迟提取请求；上传代理在旧图结束后才可销毁。
    extractions_.clear();
    parameterResources_.clear();
    passNames_.clear();
    for (const auto& resource : resources.All()) Import(graph, *resource);
}

void GraphExecutor::Import(RenderGraph& graph, GpuResource& resource) {
    // 重复显式导入仍为每份外部分配保留唯一图身份。
    const auto existing = std::find(resources_.begin(), resources_.end(), &resource);
    if (existing != resources_.end()) {
        resource.graphId = static_cast<ResourceId>(existing - resources_.begin());
        resource.graphOwner = &graph;
        return;
    }
    resource.graphId = graph.Import(resource.name, resource.state);
    resource.graphOwner = &graph;
    if (resource.graphId != resources_.size()) throw std::logic_error("GraphExecutor import order mismatch");
    resources_.push_back(&resource);
}

void GraphExecutor::Create(RenderGraph& graph, GpuResource& resource, const std::string& name, const ResourceDescriptor& description) {
    if (!pool_) throw std::logic_error("GraphExecutor transient creation requires a resource pool");
    // 物理存储创建之前，Pass 即可捕获地址稳定的逻辑代理。
    const auto resourceName = name;
    resource = {};
    resource.name = resourceName;
    resource.graphId = graph.Create(resourceName, description);
    resource.graphOwner = &graph;
    if (resource.graphId != resources_.size()) throw std::logic_error("GraphExecutor create order mismatch");
    resources_.push_back(&resource);
}

void GraphExecutor::Extract(RenderGraph& graph, const char* name, GpuResource& source, GpuResource& destination,
    D3D12_RESOURCE_STATES finalState) {
    // 拷贝提取保持外部历史地址稳定，并明确跨帧所有权边界。
    PassParameters parameters;
    parameters.Access(source, ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    parameters.Access(destination, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    parameters.AddPass(graph, name,
        [this, &source, &destination](const PassParameters&) { device_.Commands()->CopyResource(destination.object.Get(), source.object.Get()); });
    graph.Export(destination.graphId, finalState);
}

GpuResource& GraphExecutor::UploadParameters(RenderGraph& graph, const std::string& passName, std::span<const std::byte> bytes) {
    if (bytes.empty()) throw std::invalid_argument("RDG shader parameter upload cannot be empty");
    ResourceDescriptor description{ResourceKind::Buffer, (bytes.size() + 255ull) & ~255ull};
    description.memory = ResourceMemory::Upload;
    auto& buffer = parameterResources_.emplace_back();
    Create(graph, buffer, passName + ".Parameters", description);
    PassParameters parameters;
    parameters.Access(buffer, ResourceAccess::Write, D3D12_RESOURCE_STATE_GENERIC_READ);
    // CPU 数据先按值交给图；物理上传分配与 Map 写入都推迟到保留 Pass 的执行阶段。
    parameters.AddPass(graph, "Upload." + passName,
        [&buffer, values = std::vector<std::byte>(bytes.begin(), bytes.end())](const PassParameters&) {
            void* mapped{};
            const D3D12_RANGE readRange{0, 0};
            Check(buffer.object->Map(0, &readRange, &mapped), "Map pass shader parameters");
            std::memcpy(mapped, values.data(), values.size());
            const D3D12_RANGE written{0, values.size()};
            buffer.object->Unmap(0, &written);
        });
    return buffer;
}

void GraphExecutor::QueueExtraction(RenderGraph& graph, const char* name, GpuResource& source, GpuResource& destination,
    D3D12_RESOURCE_STATES finalState) {
    if (!pool_ || source.graphOwner != &graph || destination.graphOwner != &graph ||
        source.graphId >= graph.Resources().size() || !graph.Resources()[source.graphId].transient || !destination.object)
        throw std::invalid_argument("RDG queued extraction requires a transient source and a registered external destination");
    // 在改图之前拒绝不兼容目标，避免多项提取只发布了前半部分。
    pool_->ValidateExtraction(graph.Resources()[source.graphId].description, destination);
    if (std::any_of(extractions_.begin(), extractions_.end(), [&](const auto& extraction) {
        return extraction.source == source.graphId || extraction.destination == &destination;
    })) throw std::invalid_argument("RDG extraction source and destination must each be unique");
    // 提取只声明末端访问和所有权边界，不录制 CopyResource，也不在构图时填充外部代理。
    PassParameters parameters;
    parameters.Access(source, ResourceAccess::Read, finalState);
    parameters.AddPass(graph, name, [](const PassParameters&) {}, true);
    graph.Export(source.graphId, finalState);
    extractions_.push_back({source.graphId, &destination});
}

void GraphExecutor::Execute(RenderGraph& graph) {
    // 图编译属于 CPU 工作，统计时与原生命令录制分开。
    using Clock = std::chrono::steady_clock;
    const auto compileStart = profiler_ ? Clock::now() : Clock::time_point{};
    graph.Compile();
    if (resources_.size() != graph.Resources().size()) throw std::logic_error("RDG contains resources not registered with its executor");
    // 外部资源可能在建图之后被替换；所有提取必须在录制和任何所有权交换前通过复检。
    for (const auto& extraction : extractions_)
        pool_->ValidateExtraction(graph.Resources().at(extraction.source).description, *extraction.destination);
    if (profiler_) profiler_->BeginFrame(graph.Plan().size(),
        std::chrono::duration<double, std::milli>(Clock::now() - compileStart).count());
    passNames_.clear();
    for (const auto& pass : graph.Plan()) passNames_.push_back(pass.name);
    std::vector<GpuResource*> allocations(graph.Allocations().size());
    // 仅在此处修改原生状态，保持跨帧资源状态历史一致。
    graph.Execute([&](const Barrier& barrier) {
        // 关闭录制时连 CPU 读时钟也跳过，避免屏障数量放大空闲采集开销。
        const auto barrierStart = profiler_ ? Clock::now() : Clock::time_point{};
        auto& resource = *resources_.at(barrier.resource);
        D3D12_RESOURCE_BARRIER native{};
        if (barrier.uav) {
            native.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            native.UAV.pResource = resource.object.Get();
        } else {
            native.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            native.Transition.pResource = resource.object.Get();
            native.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(barrier.before);
            native.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(barrier.after);
            native.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            resource.state = native.Transition.StateAfter;
        }
        device_.Commands()->ResourceBarrier(1, &native);
        if (profiler_) profiler_->RecordBarrier(
            std::chrono::duration<double, std::milli>(Clock::now() - barrierStart).count());
    }, [&](ResourceId id, bool acquire) {
        auto& logical = *resources_.at(id);
        auto& physical = allocations.at(graph.Lifetimes()[id].allocation);
        if (acquire) {
            if (!physical) physical = &pool_->AcquireTransient(graph.Resources()[id].description, logical.name);
            // 回收资源以 COMMON 进入新的逻辑寿命，与编译屏障保持一致。
            if (graph.Resources()[id].description.memory == ResourceMemory::Default && physical->state != D3D12_RESOURCE_STATE_COMMON) {
                const auto barrierStart = profiler_ ? Clock::now() : Clock::time_point{};
                D3D12_RESOURCE_BARRIER reset{};
                reset.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                reset.Transition.pResource = physical->object.Get();
                reset.Transition.StateBefore = physical->state;
                reset.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                reset.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                device_.Commands()->ResourceBarrier(1, &reset);
                physical->state = D3D12_RESOURCE_STATE_COMMON;
                if (profiler_) profiler_->RecordBarrier(
                    std::chrono::duration<double, std::milli>(Clock::now() - barrierStart).count());
            }
            const auto graphId = logical.graphId;
            const auto name = logical.name;
            logical = *physical;
            logical.graphId = graphId;
            logical.graphOwner = &graph;
            logical.name = name;
        } else {
            physical->state = logical.state;
            // 清空代理，便于发现声明寿命之外的意外原生访问。
            logical.object.Reset();
            logical.srv = logical.uav = logical.clearUav = 0;
        }
    }, [&](const CompiledPass& pass) {
        // 平台无关执行钩子保留图裁剪和调度后的 Pass 顺序。
        if (profiler_) profiler_->BeginPass(pass);
    }, [&](const CompiledPass&) {
        if (profiler_) profiler_->EndPass();
    });
    // 全部 Pass 和最终状态转换成功后才发布历史；失败的图不会替换外部结果。
    for (const auto& extraction : extractions_) {
        auto* physical = allocations.at(graph.Lifetimes().at(extraction.source).allocation);
        pool_->ExchangeTransient(*physical, *extraction.destination);
    }
    if (profiler_) profiler_->EndFrame();
}
}
