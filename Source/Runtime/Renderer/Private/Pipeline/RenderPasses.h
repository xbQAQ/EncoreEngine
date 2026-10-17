#pragma once
// 内置 Pass 的组合契约属于管线层，各算法实现保留自己的目录。
#include "render/RenderGraph.h"

namespace render {
namespace dx12 { class GraphExecutor; }
class PassContext;
struct RenderSettings;
// 各算法入口在独立实现文件中声明自身 RDG 依赖。
void AddGBufferPass(RenderGraph&, PassContext&);
void AddDirectLightingPass(RenderGraph&, PassContext&);
void AddPathTracingPass(RenderGraph&, PassContext&);
void AddSSGIPass(RenderGraph&, PassContext&);
void AddReSTIRGIPasses(RenderGraph&, PassContext&);
void AddRadianceResolvePasses(RenderGraph&, PassContext&);
// 屏幕效果只消费已解析 HDR，并返回本帧显示输入的显式依赖链。
void AddScreenSpaceEffectsPasses(RenderGraph&, PassContext&, const RenderSettings&);
void AddPostProcessPass(RenderGraph&, PassContext&);
void AddFrameHistoryPasses(RenderGraph&, PassContext&, dx12::GraphExecutor&, unsigned giMode);
}
