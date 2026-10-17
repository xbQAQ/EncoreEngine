#pragma once
#include <functional>

namespace render {
class PassContext;
class ShadowRenderer;
class RenderPipeline;
class RenderGraph;
struct RenderPipelineContext;

// 固定的延迟渲染入口负责阶段顺序；扩展只在公共阶段注册建图工作。
void DeferredRender(PassContext&, ShadowRenderer&, RenderPipeline&, RenderPipelineContext&,
    const std::function<void(RenderGraph&)>& finalOverlay);
}
