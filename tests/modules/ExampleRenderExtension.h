#pragma once
#include "render/RenderPipeline.h"
#include "dx12/Dx12Common.h"
#include <array>
#include <cstdint>

namespace render::tests {
// 独立静态模块只消费公共 Renderer/RHI 契约，注册对象必须活到注销和图执行结束。
class ExampleRenderExtension final : public IRenderExtension {
public:
    explicit ExampleRenderExtension(dx12::Dx12Device& device);
    ExampleRenderExtension(const ExampleRenderExtension&) = delete;
    ExampleRenderExtension& operator=(const ExampleRenderExtension&) = delete;
    ExampleRenderExtension(ExampleRenderExtension&&) = delete;
    ExampleRenderExtension& operator=(ExampleRenderExtension&&) = delete;

    void RegisterPasses(RenderPipeline& pipeline) override;
    void UnregisterPasses(RenderPipeline& pipeline);
    // 调用者必须先等待对应帧的 GPU 提交完成，再读取独立模块拥有的结果。
    std::array<std::uint32_t, 8> Readback() const;
    static std::array<std::uint32_t, 8> ExpectedValues(unsigned width, unsigned height);
    dx12::GpuResource& Output() { return output_; }

private:
    void AddParentPass(RenderPipelineContext& context);
    void AddChildPass(RenderPipelineContext& context);

    dx12::GpuResource output_; // 父 Pass 完整写入的默认堆缓冲，子 Pass 将其复制到读回结果。
    dx12::GpuResource readback_; // 子 Pass 完整写入的两组标记，寿命不依赖 Renderer 私有帧资源。
    RenderPassHandle parent_; // 一次注销即可移除父 Pass 及全部子 Pass。
};
}
