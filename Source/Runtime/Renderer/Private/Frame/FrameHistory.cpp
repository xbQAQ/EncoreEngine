// 历史提交属于帧边界，通过资源所有者执行跨帧提取。
#include "Pipeline/RenderPasses.h"
#include "Pipeline/PassContext.h"
#include "Resources/FrameResources.h"

namespace render {
void AddFrameHistoryPasses(RenderGraph& graph, PassContext& context, dx12::GraphExecutor& executor, unsigned giMode) {
    // 历史所有者统一声明跨帧提取，帧组合层不逐项维护纹理和缓冲区副本。
    context.targets.history.QueueExtraction(graph, executor, context.targets, giMode);
}
}
