#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace render {
class RenderGraph;
struct RenderSettings;
namespace dx12 {
class GraphExecutor;
class Dx12Device;
class DescriptorArena;
struct GpuResource;
}

// 固定阶段只决定建图入口，具体 GPU 依赖仍由 RDG 的资源读写声明建立。
enum class RenderStage {
    Prepare, // 场景上传、帧初始化及其他算法运行之前的资源准备。
    Shadows, // 光源阴影图生成及阴影滤波。
    BasePass, // 可见表面的基础属性与深度输出。
    DirectLighting, // 可见表面的直接光照与自发光求值。
    GlobalIllumination, // 当前选定全局光照算法的建图入口。
    Denoise, // 辐亮度历史重建与空间降噪。
    ScreenEffects, // 读取已重建 HDR 的屏幕空间效果。
    PostProcess, // 曝光、泛光、显示变换及最终颜色输出。
    History, // 为后续帧提取或复制当前帧的历史资源。
    Overlay, // 最终编辑器或运行时覆盖层。
};

// 同一位置的扩展严格按注册顺序执行，每个节点先建图再递归执行子节点。
enum class PassLocation {
    Before, // 位于内置阶段入口及其子 Pass 之前。
    After, // 位于内置阶段入口及其子 Pass 之后。
};

// 返回稳定的阶段调试名称；非法枚举值会抛出 std::invalid_argument。
const char* RenderStageName(RenderStage stage);

// 句柄同时携带管线身份和永不复用的注册编号，防止跨管线或旧句柄误操作。
class RenderPassHandle {
public:
    RenderPassHandle() = default;
    explicit operator bool() const { return owner_ != 0 && token_ != 0; }
    bool operator==(const RenderPassHandle&) const = default;

private:
    friend class RenderPipeline;
    RenderPassHandle(std::uint64_t owner, std::uint64_t token) : owner_(owner), token_(token) {}
    std::uint64_t owner_{}; // 创建此句柄的唯一管线身份，析构后也不会分配给新管线。
    std::uint64_t token_{}; // 当前管线内单调增长的节点编号，卸载后不会重新使用。
};

// 仅在建图期间借用帧数据；延迟执行的 RDG 回调必须自行保存所需值和稳定资源引用。
struct RenderPipelineContext {
    RenderPipelineContext(RenderGraph& graph, unsigned width, unsigned height, const RenderSettings& settings);
    RenderPipelineContext(const RenderPipelineContext&) = delete;
    RenderPipelineContext& operator=(const RenderPipelineContext&) = delete;

    // CPU 建图测试无需 RHI；真实帧在执行阶段前注入完整的原生接口。
    void AttachRhi(dx12::GraphExecutor& executor, dx12::Dx12Device& device, dx12::DescriptorArena& descriptors);
    dx12::GraphExecutor& Executor() const;
    dx12::Dx12Device& Device() const;
    dx12::DescriptorArena& Descriptors() const;

    // 黑板不拥有资源；发布的代理及其原生分配必须覆盖整次图执行的寿命。
    void Publish(std::string name, dx12::GpuResource& resource);
    dx12::GpuResource* Find(std::string_view name) const;
    dx12::GpuResource& Require(std::string_view name) const;
    const std::string& PassPath() const { return passPath_; }

    RenderGraph& graph; // 当前帧的 RDG，扩展通过参数声明向其中添加 Pass。
    unsigned width; // 当前渲染视图的像素宽度。
    unsigned height; // 当前渲染视图的像素高度。
    const RenderSettings& settings; // 本帧不可变的渲染配置，不暴露 Renderer 私有帧资源。

private:
    friend class RenderPipeline;
    dx12::GraphExecutor* executor_{}; // 可选的实际 RDG 后端，缺失时必需访问器明确抛错。
    dx12::Dx12Device* device_{}; // 外部宿主持有的实际设备，不在 CPU 测试中伪造引用。
    dx12::DescriptorArena* descriptors_{}; // 当前帧可用的描述符分配入口。
    std::unordered_map<std::string, dx12::GpuResource*> resources_; // 跨阶段共享的非拥有资源黑板。
    std::string passPath_; // 当前建图回调的层级路径，阶段执行结束或异常后恢复。
    bool executing_{}; // 防止同一上下文被多个管线的嵌套执行覆盖。
};

// 公共注册接口可由独立模块直接使用；所有调用限定在同一建图线程。
class RenderPipeline {
public:
    using SetupCallback = std::function<void(RenderPipelineContext&)>;

    RenderPipeline();
    ~RenderPipeline();
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;

    RenderPassHandle GetStagePass(RenderStage stage) const;
    RenderPassHandle RegisterPass(RenderStage stage, PassLocation location, std::string name, SetupCallback setup);
    RenderPassHandle RegisterSubpass(RenderPassHandle parent, std::string name, SetupCallback setup);
    // 同管线已卸载句柄返回 false；空句柄、外部管线句柄和内置阶段根节点会抛错。
    bool Unregister(RenderPassHandle handle);
    // 只执行建图回调；阶段前扩展、内置入口、根子 Pass、阶段后扩展按固定顺序展开。
    void ExecuteStage(RenderStage stage, RenderPipelineContext& context, const SetupCallback& builtinSetup = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_; // 隐藏节点存储与执行锁，保持模块调用者只依赖公共契约。
};

// 模块负责保存注册句柄，并在捕获对象销毁之前递归注销对应节点。
// 注销只影响之后的建图；已建图回调执行结束之前不得卸载其动态模块或释放依赖。
class IRenderExtension {
public:
    virtual ~IRenderExtension() = default;
    virtual void RegisterPasses(RenderPipeline& pipeline) = 0;
};
}
