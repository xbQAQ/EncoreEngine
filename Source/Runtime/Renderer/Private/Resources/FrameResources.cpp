// 帧资源声明与提取集中在资源层，分配和执行仍委托 RHI。
#include "Resources/FrameResources.h"
#include "dx12/DescriptorArena.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"

namespace render {
namespace {
// 所有相机图像保持原格式与尺寸，能力由具体用途单独提供。
ResourceDescriptor TextureDescription(unsigned width, unsigned height, D3D12_RESOURCE_FLAGS flags,
    DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT) {
    return {ResourceKind::Texture2D, width, height, static_cast<unsigned>(format), 0, static_cast<unsigned>(flags), {}};
}

// 每像素 reservoir 是六个 float4，共 96 字节，不能按二维纹理描述分配。
ResourceDescriptor ReservoirDescription(unsigned width, unsigned height) {
    return {ResourceKind::Buffer, static_cast<std::uint64_t>(width) * height * 96, 1, 0, 96,
        static_cast<unsigned>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), {}};
}
constexpr auto writable = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
constexpr auto rasterFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | writable;
}

GBufferResources::GBufferResources(unsigned width, unsigned height)
    : position("GBuffer.Position", TextureDescription(width, height, rasterFlags)),
      normal("GBuffer.Normal", TextureDescription(width, height, rasterFlags)),
      base("GBuffer.BaseColor", TextureDescription(width, height, rasterFlags)),
      tangent("GBuffer.Tangent", TextureDescription(width, height, rasterFlags)),
      uv("GBuffer.UV", TextureDescription(width, height, rasterFlags)),
      depth("GBuffer.Depth", TextureDescription(width, height, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, DXGI_FORMAT_D32_FLOAT)) {}

void GBufferResources::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 五张颜色附件与一张深度附件始终由可见表面阶段统一声明。
    for (auto* target : {&position, &normal, &base, &tangent, &uv, &depth}) target->Declare(graph, executor);
}

LightingResources::LightingResources(unsigned width, unsigned height)
    : direct("Lighting.Direct", TextureDescription(width, height, writable)),
      gi("Lighting.GI", TextureDescription(width, height, writable)) {}

void LightingResources::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 积分器未生产的计算纹理由图裁剪，不提前分配 GPU 存储。
    direct.Declare(graph, executor);
    gi.Declare(graph, executor);
}

ReSTIRResources::ReSTIRResources(unsigned width, unsigned height)
    : candidates("ReSTIR.Candidates", ReservoirDescription(width, height)),
      temporal("ReSTIR.Temporal", ReservoirDescription(width, height)),
      spatial("ReSTIR.Spatial", ReservoirDescription(width, height)) {}

void ReSTIRResources::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 各阶段保留独立逻辑值，让 RDG 根据真实读取区间决定缓冲区复用。
    candidates.Declare(graph, executor);
    temporal.Declare(graph, executor);
    spatial.Declare(graph, executor);
}

DenoiserResources::DenoiserResources(unsigned width, unsigned height)
    : resolved("Resolve.Temporal", TextureDescription(width, height, writable)),
      filtered("Resolve.Spatial", TextureDescription(width, height, writable)) {}

void DenoiserResources::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 邻域滤波保持输入输出分离，避免原地读写竞争。
    resolved.Declare(graph, executor);
    filtered.Declare(graph, executor);
}

ScreenEffectResources::ScreenEffectResources(unsigned width, unsigned height)
    : sky("Effects.Sky", TextureDescription(width, height, writable)),
      ambientOcclusion("Effects.SSAO", TextureDescription(width, height, writable)),
      reflections("Effects.SSR", TextureDescription(width, height, writable)),
      indirectLight("Effects.SSIL", TextureDescription(width, height, writable)),
      subsurfaceHorizontal("Effects.SSS.Horizontal", TextureDescription(width, height, writable)),
      subsurface("Effects.SSS", TextureDescription(width, height, writable)) {}

void ScreenEffectResources::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 关闭的效果没有保留 Pass，因此这些逻辑描述不会产生物理分配。
    for (auto* texture : {&sky, &ambientOcclusion, &reflections, &indirectLight, &subsurfaceHorizontal, &subsurface})
        texture->Declare(graph, executor);
}

FrameResources::FrameResources(dx12::DescriptorArena& descriptors, dx12::ResourcePool& pool, unsigned widthValue, unsigned heightValue)
    : width(widthValue), height(heightValue), gbuffer(width, height), lighting(width, height), restir(width, height),
      denoiser(width, height), effects(width, height), history(pool, width, height),
      diagnostics_{{{&gbuffer.position, nullptr}, {&gbuffer.normal, nullptr}, {&gbuffer.base, nullptr},
          {&gbuffer.uv, nullptr}, {&lighting.direct, nullptr}, {&denoiser.filtered, nullptr}}} {
    // 视口图像跨图可见，效果链的入口则指向当前帧空间滤波结果。
    display = &pool.Texture("Post.Display", width, height, DXGI_FORMAT_R8G8B8A8_UNORM, writable);
    descriptors.CopyToVisible(1, display->srv);
    presentation = denoiser.filtered.Pointer();
}

void FrameResources::PrepareDiagnostics(dx12::ResourcePool& pool) {
    // 可选长期快照保留现有读回行为，正常帧不为诊断图像分配存储。
    if (!retainFrameImages_) return;
    for (auto& image : diagnostics_) {
        if (image.snapshot) continue;
        const auto& description = image.source->Description();
        image.snapshot = &pool.Texture(image.source->Resource().name, static_cast<unsigned>(description.width), description.height,
            static_cast<DXGI_FORMAT>(description.format), writable);
    }
}

void FrameResources::DeclareFrame(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 各组自己登记资源，帧协调层不维护混合纹理与缓冲区的固定索引数组。
    gbuffer.Declare(graph, executor);
    lighting.Declare(graph, executor);
    restir.Declare(graph, executor);
    denoiser.Declare(graph, executor);
    effects.Declare(graph, executor);
}

void FrameResources::ExtractDiagnostics(RenderGraph& graph, dx12::GraphExecutor& executor, unsigned giMode) {
    // 路径追踪没有独立直接光生产者，因此跳过该快照并保留其他图像检查。
    for (auto& image : diagnostics_)
        if (image.snapshot && (giMode != 0 || image.source != &lighting.direct))
            executor.Extract(graph, ("Inspect." + image.source->Resource().name).c_str(), image.source->Resource(), *image.snapshot);
}
}
