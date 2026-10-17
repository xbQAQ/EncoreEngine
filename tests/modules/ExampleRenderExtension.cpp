#include "ExampleRenderExtension.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/PassParameters.h"
#include <cstring>
#include <span>
#include <stdexcept>

namespace render::tests {
namespace {
// 模块自行拥有物理分配，宿主无需把扩展资源加入 Renderer 的私有资源池。
dx12::GpuResource CreateBuffer(dx12::Dx12Device& device, const char* name, UINT64 bytes,
    D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState) {
    dx12::GpuResource buffer;
    buffer.name = name;
    buffer.state = initialState;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    dx12::Check(device.Native()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        initialState, nullptr, IID_PPV_ARGS(&buffer.object)), "Create external module buffer");
    return buffer;
}
}

ExampleRenderExtension::ExampleRenderExtension(dx12::Dx12Device& device)
    : output_(CreateBuffer(device, "ExampleExtension.Output", 4 * sizeof(std::uint32_t),
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON)),
      readback_(CreateBuffer(device, "ExampleExtension.Readback", 8 * sizeof(std::uint32_t),
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)) {}

void ExampleRenderExtension::RegisterPasses(RenderPipeline& pipeline) {
    // 模块保存根句柄；子节点注册失败时撤销父节点，避免留下捕获当前对象的回调。
    if (parent_) throw std::logic_error("Example extension is already registered");
    parent_ = pipeline.RegisterPass(RenderStage::PostProcess, PassLocation::After, "Example.Copy",
        [this](RenderPipelineContext& context) { AddParentPass(context); });
    try {
        pipeline.RegisterSubpass(parent_, "Readback",
            [this](RenderPipelineContext& context) { AddChildPass(context); });
    } catch (...) {
        pipeline.Unregister(parent_);
        parent_ = {};
        throw;
    }
}

void ExampleRenderExtension::UnregisterPasses(RenderPipeline& pipeline) {
    if (!parent_) return;
    pipeline.Unregister(parent_);
    parent_ = {};
}

std::array<std::uint32_t, 8> ExampleRenderExtension::ExpectedValues(unsigned width, unsigned height) {
    // 两组不同标记区分父子上传，视图尺寸证明参数来自当前公共上下文。
    return {0x1234abcdu, width, height, 0xcafe0011u, 0x8fedcba9u, height, width, 0xbabe0022u};
}

void ExampleRenderExtension::AddParentPass(RenderPipelineContext& context) {
    auto& executor = context.Executor();
    auto& display = context.Require("Post.Display");
    executor.Import(context.graph, output_);
    executor.Import(context.graph, readback_);
    const auto values = ExpectedValues(context.width, context.height);
    auto& upload = executor.UploadParameters(context.graph, context.PassPath(),
        std::as_bytes(std::span(values).first<4>()));

    // 声明显示输出依赖以及完整缓冲写入，所有原生命令仅发生在参数校验之后。
    dx12::PassParameters parameters;
    parameters.Access(display, ResourceAccess::Read, dx12::ShaderRead);
    parameters.Constants(upload);
    parameters.Access(output_, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    auto* device = &context.Device();
    auto* output = &output_;
    parameters.AddPass(context.graph, context.PassPath(), [device, output](const dx12::PassParameters& declared) {
        device->Commands()->CopyBufferRegion(output->object.Get(), 0,
            declared.ConstantBuffer()->object.Get(), 0, 4 * sizeof(std::uint32_t));
    });
}

void ExampleRenderExtension::AddChildPass(RenderPipelineContext& context) {
    const auto values = ExpectedValues(context.width, context.height);
    auto& upload = context.Executor().UploadParameters(context.graph, context.PassPath(),
        std::as_bytes(std::span(values).last<4>()));

    // 子 Pass 独立上传参数，并把父输出与自身参数完整组合为可定量验证的读回缓冲。
    dx12::PassParameters parameters;
    parameters.Access(output_, ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    parameters.Constants(upload);
    parameters.Access(readback_, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    auto* device = &context.Device();
    auto* output = &output_;
    auto* readback = &readback_;
    parameters.AddPass(context.graph, context.PassPath(), [device, output, readback](const dx12::PassParameters& declared) {
        constexpr auto bytes = 4 * sizeof(std::uint32_t);
        device->Commands()->CopyBufferRegion(readback->object.Get(), 0, output->object.Get(), 0, bytes);
        device->Commands()->CopyBufferRegion(readback->object.Get(), bytes,
            declared.ConstantBuffer()->object.Get(), 0, bytes);
    });
    context.graph.Export(readback_.graphId, D3D12_RESOURCE_STATE_COPY_DEST);
}

std::array<std::uint32_t, 8> ExampleRenderExtension::Readback() const {
    // Map 只读取同步完成的外部读回存储，不访问已释放的瞬时上传代理。
    std::array<std::uint32_t, 8> values{};
    const D3D12_RANGE readRange{0, sizeof(values)};
    void* mapped{};
    dx12::Check(readback_.object->Map(0, &readRange, &mapped), "Map external module readback");
    std::memcpy(values.data(), mapped, sizeof(values));
    const D3D12_RANGE written{0, 0};
    readback_.object->Unmap(0, &written);
    return values;
}
}
