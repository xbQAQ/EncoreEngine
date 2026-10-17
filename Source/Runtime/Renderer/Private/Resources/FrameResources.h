#pragma once
// 图内资源与视图历史分属不同寿命，由帧资源集合显式组合。
#include "Resources/GraphResources.h"
#include "Resources/RenderTarget.h"
#include "View/ViewHistory.h"
#include <array>

namespace render::dx12 { class DescriptorArena; class ResourcePool; class GraphExecutor; }
namespace render {
struct GBufferResources {
    GBufferResources(unsigned width, unsigned height);
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    RenderTarget position; // 光栅世界位置及材质身份的颜色附件。
    RenderTarget normal; // 光栅法线与感知粗糙度的颜色附件。
    RenderTarget base; // 光栅基础颜色与金属度的颜色附件。
    RenderTarget tangent; // 着色切线与 UV 手性的颜色附件。
    RenderTarget uv; // 纹理 UV、图元身份与正面标记的颜色附件。
    RenderTarget depth; // 光栅深度测试使用的深度附件。
};

struct LightingResources {
    LightingResources(unsigned width, unsigned height);
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    GraphTexture direct; // 主表面直接光和自发光的计算输出。
    GraphTexture gi; // 当前间接光或完整路径追踪辐射的计算输出。
};

struct ReSTIRResources {
    ReSTIRResources(unsigned width, unsigned height);
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    GraphBuffer candidates; // 每像素新生成的次级表面 reservoir。
    GraphBuffer temporal; // 时间复用结果，供空间复用阶段只读访问。
    GraphBuffer spatial; // 最终空间复用结果，供求值与历史提取访问。
};

struct DenoiserResources {
    DenoiserResources(unsigned width, unsigned height);
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    GraphTexture resolved; // 时间重建后的线性辐射和置信度。
    GraphTexture filtered; // 空间滤波后的线性辐射。
};

struct ScreenEffectResources {
    ScreenEffectResources(unsigned width, unsigned height);
    void Declare(RenderGraph&, dx12::GraphExecutor&);
    GraphTexture sky; // 天空合成后的 HDR 计算输出。
    GraphTexture ambientOcclusion; // 屏幕空间遮蔽合成结果。
    GraphTexture reflections; // 屏幕空间反射合成结果。
    GraphTexture indirectLight; // 独立屏幕空间间接光合成结果。
    GraphTexture subsurfaceHorizontal; // 次表面扩散横向滤波的中间结果。
    GraphTexture subsurface; // 次表面扩散纵向合成结果。
};

// 帧级协调对象按算法职责聚合资源，不把纹理、缓冲区或历史称为 RenderTarget。
class FrameResources {
public:
    FrameResources(dx12::DescriptorArena&, dx12::ResourcePool&, unsigned width, unsigned height);
    void PrepareDiagnostics(dx12::ResourcePool&);
    void DeclareFrame(RenderGraph&, dx12::GraphExecutor&);
    void ExtractDiagnostics(RenderGraph&, dx12::GraphExecutor&, unsigned giMode);
    void RetainFrameImages() { retainFrameImages_ = true; }
    unsigned width; // 独立于编辑器窗口缩放的内部渲染宽度。
    unsigned height; // 纹理与每像素 reservoir 共用的内部渲染高度。
    GBufferResources gbuffer; // 当前可见表面的颜色附件与深度附件。
    LightingResources lighting; // 当前帧直接光和间接光的计算纹理。
    ReSTIRResources restir; // 当前帧各复用阶段的结构缓冲区。
    DenoiserResources denoiser; // 时间重建与空间滤波的计算纹理。
    ScreenEffectResources effects; // 按开关决定是否参与图执行的屏幕效果纹理。
    ViewHistory history; // 独立管理的跨帧视图历史。
    dx12::GpuResource* display{}; // 色调映射和 sRGB 编码后的长期视口图像。
    dx12::GpuResource* presentation{}; // 本帧最后一个已启用效果的 HDR 输出。
private:
    struct DiagnosticImage {
        GraphTexture* source{}; // 被检查的单张图内纹理，不包含任何结构缓冲区。
        dx12::GpuResource* snapshot{}; // 可选长期读回快照，不延长原纹理的正常寿命。
    };
    std::array<DiagnosticImage, 6> diagnostics_; // 仅为明确支持检查的图像保留可选快照。
    bool retainFrameImages_{}; // 按需启用诊断提取，不为正常帧增加外部图像。
};
}
