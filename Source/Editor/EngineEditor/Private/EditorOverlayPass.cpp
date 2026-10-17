#include "editor/EditorUi.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"

namespace render::editor {
void EditorUi::AddOverlayPass(RenderGraph& graph, dx12::GpuResource& backbuffer,
    dx12::GpuResource& display, unsigned backbufferIndex) {
    const auto overlay = graph.AddPass("Editor.ImGui", {dx12::Read(display), dx12::Write(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET)},
        [this, backbufferIndex] {
            const auto target = descriptors_.Rtv(backbufferIndex);
            const float clear[]{0.08f, 0.085f, 0.09f, 1};
            device_.Commands()->ClearRenderTargetView(target, clear, 0, nullptr);
            device_.Commands()->OMSetRenderTargets(1, &target, FALSE, nullptr);
            Draw(device_.Commands());
        });
    // 覆盖层位于全部保留的场景、后处理和历史工作之后，但不能保活已失去消费者的分支。
    for (PassId previous = 0; previous < overlay; ++previous) graph.AddOrdering(previous, overlay);
}
}
