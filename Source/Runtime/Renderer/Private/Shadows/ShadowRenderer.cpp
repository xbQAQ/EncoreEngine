#include "ShadowRenderer.h"
// 阴影实现借用管线缓存与上下文，不反向依赖帧渲染器。
#include "Pipeline/PassContext.h"
#include "Pipeline/PipelineCache.h"
#include "Scene/SceneGpu.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"
#include <stdexcept>

namespace render {
ShadowRenderer::ShadowRenderer(dx12::Dx12Device& device, dx12::DescriptorArena& descriptors,
    dx12::ResourcePool& pool, unsigned resolution, unsigned tiles) : resolution_(resolution), tiles_(tiles) {
    if (!resolution || !tiles || std::uint64_t(resolution) * tiles > 16384)
        throw std::invalid_argument("Shadow atlas extent exceeds D3D12 texture capacity");
    const unsigned extent = resolution * tiles;
    constexpr auto writable = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // 原始深度矩与模糊结果分开，PCSS 必须搜索未滤波的最近表面深度。
    moments_ = &pool.Texture("Shadow.Moments", extent, extent, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | writable, {1, 1, 0, 1});
    depth_ = &pool.Texture("Shadow.Depth", extent, extent, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    horizontal_ = &pool.Texture("Shadow.BlurHorizontal", extent, extent, DXGI_FORMAT_R32G32B32A32_FLOAT, writable);
    filtered_ = &pool.Texture("Shadow.BlurVertical", extent, extent, DXGI_FORMAT_R32G32B32A32_FLOAT, writable);
    device.Native()->CreateRenderTargetView(moments_->object.Get(), nullptr, descriptors.Rtv(7));
    device.Native()->CreateDepthStencilView(depth_->object.Get(), nullptr, descriptors.Dsv(1));
}

dx12::GpuResource& ShadowRenderer::Atlas(unsigned shadowMode) const {
    if (shadowMode > 3) throw std::invalid_argument("Unknown shadow filter mode");
    return *(shadowMode == 3 ? filtered_ : moments_);
}

dx12::GpuResource& ShadowRenderer::AddPasses(RenderGraph& graph, PassContext& context, unsigned viewCount, unsigned shadowMode) {
    using namespace dx12;
    auto& result = Atlas(shadowMode);
    if (shadowMode == 0) return result;
    if (viewCount > 96 || viewCount > std::uint64_t(tiles_) * tiles_ || viewCount > context.scene.ShadowViewCount())
        throw std::invalid_argument("Shadow raster view count exceeds its uploaded atlas contract");
    // 深度光栅只需场景资产与光源视图，滤波和表面光照设置不进入此 Pass。
    auto values = context.ViewParameters();
    auto parameters = context.Parameters(graph, "Shadow.RasterDepth", values, {&context.scene.Triangles(), &context.scene.Materials(),
        &context.scene.TriangleAttributes(), &context.scene.TextureViews(), &context.scene.Texels(), &context.scene.ShadowViews()});
    parameters.Access(*moments_, ResourceAccess::Write, D3D12_RESOURCE_STATE_RENDER_TARGET);
    parameters.Access(*depth_, ResourceAccess::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    parameters.AddPass(graph, "Shadow.RasterDepth", [this, &context, &graph, viewCount](const PassParameters& bound) {
            auto* commands = context.device.Commands();
            const auto target = context.descriptors.Rtv(7);
            const auto depth = context.descriptors.Dsv(1);
            const float empty[]{1, 1, 0, 1};
            commands->ClearRenderTargetView(target, empty, 0, nullptr);
            commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            commands->OMSetRenderTargets(1, &target, FALSE, &depth);
            context.Bind(graph, bound, true);
            commands->SetPipelineState(context.pipelines.Shadow());
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // 每个视图同时占据深度矩图集和硬件深度图中的一个对应图块。
            for (unsigned index = 0; index < viewCount; ++index) {
                const unsigned x = index % tiles_ * resolution_, y = index / tiles_ * resolution_;
                const D3D12_VIEWPORT viewport{static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(resolution_), static_cast<float>(resolution_), 0, 1};
                const D3D12_RECT scissor{static_cast<LONG>(x), static_cast<LONG>(y),
                    static_cast<LONG>(x + resolution_), static_cast<LONG>(y + resolution_)};
                commands->RSSetViewports(1, &viewport);
                commands->RSSetScissorRects(1, &scissor);
                const unsigned drawConstants[]{index, 0, 0, 0};
                commands->SetGraphicsRoot32BitConstants(3, 4, drawConstants, 0);
                commands->DrawInstanced(context.scene.TriangleCount() * 3, 1, 0, 0);
            }
        });
    if (shadowMode != 3) return result;
    const unsigned extent = resolution_ * tiles_;
    const auto blur = [&](const char* label, const wchar_t* shader, GpuResource& input, GpuResource& output) {
        // 描述符延迟到 Pass 执行，构图只保存逻辑输入与输出参数。
        auto blurValues = context.ViewParameters();
        // 模糊只依赖真实图块尺寸，不能继承表面光照的固定分辨率或其他设置。
        blurValues.shadow.x = static_cast<float>(resolution_);
        auto blurParameters = context.Parameters(graph, label, blurValues, {&input}, &output, false, nullptr, nullptr, &input);
        blurParameters.AddPass(graph, label, [&context, &graph, shader, extent](const PassParameters& bound) {
                context.Bind(graph, bound);
                context.device.Commands()->SetPipelineState(context.pipelines.Compute(shader, context.backend));
                // 图集工作组按图集尺寸分派，不能沿用相机图像分辨率。
                context.device.Commands()->Dispatch((extent + 7) / 8, (extent + 7) / 8, 1);
            });
    };
    blur("Shadow.BlurHorizontal", L"ShadowBlurH", *moments_, *horizontal_);
    blur("Shadow.BlurVertical", L"ShadowBlurV", *horizontal_, *filtered_);
    return result;
}
}
