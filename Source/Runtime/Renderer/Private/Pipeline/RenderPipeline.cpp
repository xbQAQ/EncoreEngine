// 实现按管线职责归类，外部扩展继续通过公共头文件接入。
#include "render/RenderPipeline.h"
#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace render {
namespace {
constexpr std::array<const char*, 10> StageNames{
    "Prepare", "Shadows", "BasePass", "DirectLighting", "GlobalIllumination",
    "Denoise", "ScreenEffects", "PostProcess", "History", "Overlay"};

// 公共枚举可能来自外部模块的转换结果，必须先验证再用于数组索引。
std::size_t StageIndex(RenderStage stage) {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= StageNames.size()) throw std::invalid_argument("Invalid render stage");
    return index;
}

// 管线身份跨对象寿命保持唯一，计数耗尽时停止而不允许句柄身份回绕。
std::uint64_t NextOwner() {
    static std::atomic<std::uint64_t> next{1};
    auto owner = next.load(std::memory_order_relaxed);
    for (;;) {
        if (owner == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Render pipeline owner identities exhausted");
        if (next.compare_exchange_weak(owner, owner + 1, std::memory_order_relaxed)) return owner;
    }
}

// 无内置回调的阶段仍可扩展，但外部节点必须具有明确名称和建图入口。
void ValidateRegistration(const std::string& name, const RenderPipeline::SetupCallback& setup) {
    if (name.empty()) throw std::invalid_argument("Render pass name must not be empty");
    if (!setup) throw std::invalid_argument("Render pass setup callback must not be empty");
}

// 每层退出都恢复父路径，异常不会把子 Pass 的调试标签泄漏到之后的阶段。
struct PathScope {
    std::string& path; // 当前上下文中的可变调试路径。
    std::string previous; // 进入当前节点之前的完整父路径。
    ~PathScope() { path.swap(previous); }
};

// 执行锁和外部路径作为一个作用域恢复，覆盖内置入口和外部回调的异常。
struct ExecutionScope {
    bool& pipelineExecuting; // 当前管线禁止重入和修改注册树的锁。
    bool& contextExecuting; // 当前上下文禁止跨管线嵌套使用的锁。
    std::string& path; // 调用者在开始本阶段之前持有的路径存储。
    std::string previousPath; // 在任何阶段或节点标签写入之前复制的原路径。
    ~ExecutionScope() {
        path.swap(previousPath);
        contextExecuting = false;
        pipelineExecuting = false;
    }
};
}

const char* RenderStageName(RenderStage stage) {
    return StageNames[StageIndex(stage)];
}

RenderPipelineContext::RenderPipelineContext(RenderGraph& frameGraph, unsigned frameWidth,
    unsigned frameHeight, const RenderSettings& frameSettings)
    : graph(frameGraph), width(frameWidth), height(frameHeight), settings(frameSettings) {}

void RenderPipelineContext::AttachRhi(dx12::GraphExecutor& executor, dx12::Dx12Device& device,
    dx12::DescriptorArena& descriptors) {
    // 原生接口属于整次建图边界，不能在一个扩展节点执行期间切换其归属。
    if (executing_) throw std::logic_error("Cannot replace RHI interfaces during render stage setup");
    executor_ = &executor;
    device_ = &device;
    descriptors_ = &descriptors;
}

dx12::GraphExecutor& RenderPipelineContext::Executor() const {
    if (!executor_) throw std::logic_error("Render pipeline context has no RHI executor");
    return *executor_;
}

dx12::Dx12Device& RenderPipelineContext::Device() const {
    if (!device_) throw std::logic_error("Render pipeline context has no RHI device");
    return *device_;
}

dx12::DescriptorArena& RenderPipelineContext::Descriptors() const {
    if (!descriptors_) throw std::logic_error("Render pipeline context has no descriptor arena");
    return *descriptors_;
}

void RenderPipelineContext::Publish(std::string name, dx12::GpuResource& resource) {
    // 后续阶段允许替换逻辑输出，资源所有权仍归宿主或图执行器。
    if (name.empty()) throw std::invalid_argument("Published render resource name must not be empty");
    resources_.insert_or_assign(std::move(name), &resource);
}

dx12::GpuResource* RenderPipelineContext::Find(std::string_view name) const {
    const auto found = resources_.find(std::string(name));
    return found == resources_.end() ? nullptr : found->second;
}

dx12::GpuResource& RenderPipelineContext::Require(std::string_view name) const {
    if (auto* resource = Find(name)) return *resource;
    throw std::out_of_range("Required render resource is not published: " + std::string(name));
}

struct RenderPipeline::Impl {
    struct Node {
        std::uint64_t token{}; // 节点唯一注册编号，不因卸载而重新使用。
        std::string name; // 追加到父路径的局部调试名称。
        SetupCallback setup; // 只声明 RDG 工作的同步建图入口。
        std::vector<std::unique_ptr<Node>> children; // 按注册顺序拥有的递归子 Pass。
    };

    struct Stage {
        Node root; // 不可卸载的固定阶段根节点，其子节点紧随内置入口。
        std::vector<std::unique_ptr<Node>> before; // 在阶段内置入口之前依次执行的扩展子树。
        std::vector<std::unique_ptr<Node>> after; // 在阶段根子节点之后依次执行的扩展子树。
    };

    std::uint64_t owner{NextOwner()}; // 区分不同管线及相同地址析构后重建的对象。
    std::uint64_t nextToken{1}; // 下一次注册使用的编号，达到边界后拒绝新增。
    std::array<Stage, StageNames.size()> stages; // 固定阶段根和各阶段前后扩展存储。
    bool executing{}; // 整次阶段遍历期间禁止重入或改变注册树。

    Impl() {
        // 阶段节点与外部节点共享编号域，使任何合法句柄都只能对应一个节点。
        for (std::size_t index = 0; index < stages.size(); ++index) {
            stages[index].root.token = AllocateToken();
            stages[index].root.name = StageNames[index];
        }
    }

    void AssertMutable() const {
        if (executing) throw std::logic_error("Cannot change render passes during render stage setup");
    }

    std::uint64_t AllocateToken() {
        if (nextToken == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Render pass registration tokens exhausted");
        return nextToken++;
    }

    std::unique_ptr<Node> MakeNode(std::string name, SetupCallback setup) {
        auto node = std::make_unique<Node>();
        node->token = AllocateToken();
        node->name = std::move(name);
        node->setup = std::move(setup);
        return node;
    }

    // 所有节点由父树独占拥有，查找不引入可能在递归卸载后悬空的索引指针。
    static Node* FindNode(Node& node, std::uint64_t token) {
        if (node.token == token) return &node;
        for (auto& child : node.children)
            if (auto* found = FindNode(*child, token)) return found;
        return nullptr;
    }

    Node* Find(std::uint64_t token) {
        for (auto& stage : stages) {
            if (auto* found = FindNode(stage.root, token)) return found;
            for (auto& node : stage.before)
                if (auto* found = FindNode(*node, token)) return found;
            for (auto& node : stage.after)
                if (auto* found = FindNode(*node, token)) return found;
        }
        return nullptr;
    }

    // 擦除唯一拥有节点即递归销毁其回调和子树，不触及其他节点的相对顺序。
    static bool Remove(std::vector<std::unique_ptr<Node>>& nodes, std::uint64_t token) {
        for (auto node = nodes.begin(); node != nodes.end(); ++node) {
            if ((*node)->token == token) {
                nodes.erase(node);
                return true;
            }
            if (Remove((*node)->children, token)) return true;
        }
        return false;
    }

    // 深度优先建图保证父节点输出先于它的子 Pass 注册，GPU 顺序仍由 RDG 推导。
    static void ExecuteNode(const Node& node, RenderPipelineContext& context, std::string& path) {
        PathScope scope{path, path};
        path += "/" + node.name;
        node.setup(context);
        for (const auto& child : node.children) ExecuteNode(*child, context, path);
    }
};

RenderPipeline::RenderPipeline() : impl_(std::make_unique<Impl>()) {}
RenderPipeline::~RenderPipeline() = default;

RenderPassHandle RenderPipeline::GetStagePass(RenderStage stage) const {
    return {impl_->owner, impl_->stages[StageIndex(stage)].root.token};
}

RenderPassHandle RenderPipeline::RegisterPass(RenderStage stage, PassLocation location, std::string name,
    SetupCallback setup) {
    // 先拒绝执行期修改，再检查调用方输入，确保节点遍历始终保持稳定。
    impl_->AssertMutable();
    auto& entry = impl_->stages[StageIndex(stage)];
    if (location != PassLocation::Before && location != PassLocation::After)
        throw std::invalid_argument("Invalid render pass location");
    ValidateRegistration(name, setup);
    auto node = impl_->MakeNode(std::move(name), std::move(setup));
    const RenderPassHandle handle{impl_->owner, node->token};
    (location == PassLocation::Before ? entry.before : entry.after).push_back(std::move(node));
    return handle;
}

RenderPassHandle RenderPipeline::RegisterSubpass(RenderPassHandle parent, std::string name, SetupCallback setup) {
    impl_->AssertMutable();
    if (parent.owner_ != impl_->owner || parent.token_ == 0)
        throw std::invalid_argument("Render pass handle belongs to a different pipeline or is empty");
    auto* parentNode = impl_->Find(parent.token_);
    if (!parentNode) throw std::invalid_argument("Render pass parent has already been unregistered");
    ValidateRegistration(name, setup);
    auto node = impl_->MakeNode(std::move(name), std::move(setup));
    const RenderPassHandle handle{impl_->owner, node->token};
    parentNode->children.push_back(std::move(node));
    return handle;
}

bool RenderPipeline::Unregister(RenderPassHandle handle) {
    impl_->AssertMutable();
    if (handle.owner_ != impl_->owner || handle.token_ == 0)
        throw std::invalid_argument("Render pass handle belongs to a different pipeline or is empty");
    for (auto& stage : impl_->stages) {
        if (stage.root.token == handle.token_)
            throw std::invalid_argument("Built-in render stage roots cannot be unregistered");
        if (Impl::Remove(stage.before, handle.token_) || Impl::Remove(stage.root.children, handle.token_)
            || Impl::Remove(stage.after, handle.token_)) return true;
    }
    return false;
}

void RenderPipeline::ExecuteStage(RenderStage stage, RenderPipelineContext& context, const SetupCallback& builtinSetup) {
    // 上下文锁同时覆盖其他管线的嵌套调用，防止资源黑板和调试路径被重入覆盖。
    if (impl_->executing || context.executing_)
        throw std::logic_error("Render stage setup cannot be reentered");
    auto& entry = impl_->stages[StageIndex(stage)];
    ExecutionScope scope{impl_->executing, context.executing_, context.passPath_, context.passPath_};
    impl_->executing = true;
    context.executing_ = true;
    context.passPath_ = entry.root.name;

    for (const auto& node : entry.before) Impl::ExecuteNode(*node, context, context.passPath_);
    if (builtinSetup) builtinSetup(context);
    for (const auto& child : entry.root.children) Impl::ExecuteNode(*child, context, context.passPath_);
    for (const auto& node : entry.after) Impl::ExecuteNode(*node, context, context.passPath_);
}
}
