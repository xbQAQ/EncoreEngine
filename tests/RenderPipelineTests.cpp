#include "render/RenderPipeline.h"
#include "render/RenderGraph.h"
#include "render/RenderSettings.h"
#include "dx12/Dx12Common.h"
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
// 显式抛错保证 Release 构建仍会检查公共扩展协议。
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 拒绝行为必须符合指定异常类型，避免其他错误掩盖接口约束失效。
template <typename Exception, typename Function>
void Rejects(Function&& function, const char* message) {
    bool rejected = false;
    try { std::forward<Function>(function)(); }
    catch (const Exception&) { rejected = true; }
    Require(rejected, message);
}

// 测试扩展只使用公共头文件，并让图回调拥有延迟执行所需的数据。
class RecordingExtension final : public render::IRenderExtension {
public:
    explicit RecordingExtension(std::shared_ptr<std::vector<std::string>> executed)
        : executed_(std::move(executed)) {}

    void RegisterPasses(render::RenderPipeline& pipeline) override {
        const auto executed = executed_;
        handle_ = pipeline.RegisterPass(render::RenderStage::BasePass, render::PassLocation::After,
            "External", [executed](render::RenderPipelineContext& context) {
                const auto path = context.PassPath();
                context.graph.AddPass(path, {}, [executed, path] { executed->push_back(path); }, true);
            });
        pipeline.RegisterSubpass(handle_, "Nested", [executed](render::RenderPipelineContext& context) {
            const auto path = context.PassPath();
            context.graph.AddPass(path, {}, [executed, path] { executed->push_back(path); }, true);
        });
    }

    void UnregisterPasses(render::RenderPipeline& pipeline) {
        Require(pipeline.Unregister(handle_), "An external module must unregister its complete subtree");
    }

private:
    std::shared_ptr<std::vector<std::string>> executed_; // 图执行结束之前持续拥有执行轨迹。
    render::RenderPassHandle handle_; // 模块卸载时一次移除整个扩展子树。
};

// 阶段前后入口、内置入口和多级子 Pass 必须按稳定深度优先顺序建图。
void StageAndSubpassOrder() {
    render::RenderPipeline pipeline;
    render::RenderGraph graph;
    render::RenderSettings settings;
    render::RenderPipelineContext context(graph, 640, 480, settings);
    std::vector<std::string> setup;
    const auto record = [&](render::RenderPipelineContext& pass) { setup.push_back(pass.PassPath()); };
    const auto before = pipeline.RegisterPass(render::RenderStage::BasePass, render::PassLocation::Before,
        "BeforeA", record);
    const auto child = pipeline.RegisterSubpass(before, "Child", record);
    pipeline.RegisterSubpass(child, "Grandchild", record);
    pipeline.RegisterPass(render::RenderStage::BasePass, render::PassLocation::Before, "BeforeB", record);
    pipeline.RegisterSubpass(pipeline.GetStagePass(render::RenderStage::BasePass), "BuiltinChild", record);
    pipeline.RegisterPass(render::RenderStage::BasePass, render::PassLocation::After, "AfterA", record);
    pipeline.RegisterPass(render::RenderStage::BasePass, render::PassLocation::After, "AfterB", record);
    pipeline.RegisterPass(render::RenderStage::PostProcess, render::PassLocation::Before, "Separate", record);

    pipeline.ExecuteStage(render::RenderStage::BasePass, context, record);
    const std::vector<std::string> expected{
        "BasePass/BeforeA", "BasePass/BeforeA/Child", "BasePass/BeforeA/Child/Grandchild",
        "BasePass/BeforeB", "BasePass", "BasePass/BuiltinChild", "BasePass/AfterA", "BasePass/AfterB"};
    Require(setup == expected, "Stage setup and recursive subpasses must preserve registration order");
    Require(context.PassPath().empty(), "A completed stage must restore the caller's path");
    setup.clear();
    pipeline.ExecuteStage(render::RenderStage::PostProcess, context, record);
    Require(setup == std::vector<std::string>{"PostProcess/Separate", "PostProcess"},
        "Extensions must execute only at their requested stage");
}

// 真正跨阶段传递 RDG 资源，验证扩展回调产生的是可编译执行的数据依赖。
void CrossStageGraphExecution() {
    render::RenderPipeline pipeline;
    render::RenderGraph graph;
    render::RenderSettings settings;
    render::RenderPipelineContext context(graph, 320, 240, settings);
    const auto resource = graph.Import("Extension.SharedValue", 0, false);
    std::vector<std::string> executed;
    int value = 0;
    pipeline.RegisterPass(render::RenderStage::Prepare, render::PassLocation::After, "Produce",
        [resource, &value, &executed](render::RenderPipelineContext& pass) {
            Require(pass.width == 320 && pass.height == 240, "View dimensions must reach external modules");
            pass.graph.AddPass(pass.PassPath(), {{resource, 1, render::ResourceAccess::Write}}, [&value, &executed] {
                value = 7;
                executed.emplace_back("produce");
            });
        });
    pipeline.RegisterPass(render::RenderStage::PostProcess, render::PassLocation::Before, "Consume",
        [resource, &value, &executed](render::RenderPipelineContext& pass) {
            pass.graph.AddPass(pass.PassPath(), {{resource, 2, render::ResourceAccess::Read}}, [&value, &executed] {
                Require(value == 7, "Later stages must read the value produced by earlier extensions");
                executed.emplace_back("consume");
            }, true);
        });
    pipeline.ExecuteStage(render::RenderStage::Prepare, context);
    pipeline.ExecuteStage(render::RenderStage::PostProcess, context);
    Require(executed.empty(), "Stage callbacks must build the graph without executing GPU work");
    const auto& plan = graph.Compile();
    Require(plan.size() == 2 && plan[0].name == "Prepare/Produce" && plan[1].name == "PostProcess/Consume",
        "Cross-stage resources must retain and order real graph passes");
    graph.Execute([](const render::Barrier&) {});
    Require(executed == std::vector<std::string>{"produce", "consume"}, "Compiled extension work must execute");
}

// 卸载父节点会递归移除子节点，旧句柄不能误删之后注册的节点。
void HandlesAndUnregistration() {
    render::RenderPipeline pipeline;
    render::RenderPipeline other;
    render::RenderGraph graph;
    render::RenderSettings settings;
    render::RenderPipelineContext context(graph, 64, 64, settings);
    std::vector<std::string> setup;
    const auto record = [&](render::RenderPipelineContext& pass) { setup.push_back(pass.PassPath()); };
    const auto parent = pipeline.RegisterPass(render::RenderStage::History, render::PassLocation::After, "Parent", record);
    const auto child = pipeline.RegisterSubpass(parent, "Child", record);
    pipeline.RegisterSubpass(child, "Grandchild", record);
    Require(pipeline.Unregister(parent), "Registered extension must be removable");
    Require(!pipeline.Unregister(parent) && !pipeline.Unregister(child), "Removed subtree handles must stay stale");
    Rejects<std::invalid_argument>([&] { pipeline.RegisterSubpass(child, "Stale", record); },
        "A stale parent must reject new children");
    const auto replacement = pipeline.RegisterPass(render::RenderStage::History, render::PassLocation::After,
        "Replacement", record);
    Require(parent != replacement && !pipeline.Unregister(parent), "A new registration must not reuse an old handle");
    pipeline.ExecuteStage(render::RenderStage::History, context);
    Require(setup == std::vector<std::string>{"History/Replacement"}, "Removed descendants must never run");
    Rejects<std::invalid_argument>([&] { other.Unregister(replacement); }, "Foreign owners must reject unregister");
    Rejects<std::invalid_argument>([&] { other.RegisterSubpass(replacement, "Foreign", record); },
        "Foreign owners must reject subpass registration");
    Rejects<std::invalid_argument>([&] { pipeline.Unregister({}); }, "Empty handles must be rejected");
    Rejects<std::invalid_argument>([&] { pipeline.Unregister(pipeline.GetStagePass(render::RenderStage::History)); },
        "Built-in stage entry points cannot be unregistered");

    // 在完全相同的对象存储上重建管线，旧 owner 也不能命中新管线的同编号节点。
    std::optional<render::RenderPipeline> reused(std::in_place);
    const auto priorOwner = reused->GetStagePass(render::RenderStage::Prepare);
    reused.reset();
    reused.emplace();
    Rejects<std::invalid_argument>([&] { reused->RegisterSubpass(priorOwner, "PreviousOwner", record); },
        "Reusing a pipeline object's address must never reuse its handle owner identity");
}

// 异常退出必须恢复执行锁和路径，并禁止会破坏遍历的重入与增删。
void ReentrancyAndExceptionRecovery() {
    render::RenderPipeline pipeline;
    render::RenderPipeline other;
    render::RenderGraph graph;
    render::RenderSettings settings;
    render::RenderPipelineContext context(graph, 64, 64, settings);
    render::RenderPassHandle active;
    unsigned checks = 0;
    active = pipeline.RegisterPass(render::RenderStage::Denoise, render::PassLocation::After, "Guarded",
        [&](render::RenderPipelineContext& pass) {
            Rejects<std::logic_error>([&] { pipeline.RegisterPass(render::RenderStage::Prepare,
                render::PassLocation::Before, "DuringExecution", [](auto&) {}); }, "Registration during setup must fail");
            Rejects<std::logic_error>([&] { pipeline.RegisterSubpass(active, "DuringExecution", [](auto&) {}); },
                "Subpass registration during setup must fail");
            Rejects<std::logic_error>([&] { pipeline.Unregister(active); }, "Unregister during setup must fail");
            Rejects<std::logic_error>([&] { pipeline.ExecuteStage(render::RenderStage::History, pass); },
                "Recursive pipeline execution must fail");
            Rejects<std::logic_error>([&] { other.ExecuteStage(render::RenderStage::History, pass); },
                "A context cannot belong to overlapping stage executions");
            checks += 5;
            throw std::runtime_error("Intentional extension failure");
        });
    Rejects<std::runtime_error>([&] { pipeline.ExecuteStage(render::RenderStage::Denoise, context); },
        "Setup exceptions must propagate");
    Require(checks == 5 && context.PassPath().empty(), "Failure must complete guard checks and restore path");
    Require(pipeline.Unregister(active), "Failure must release the mutation guard");
    std::string recovered;
    pipeline.RegisterSubpass(pipeline.GetStagePass(render::RenderStage::Denoise), "Recovered",
        [&](auto& pass) { recovered = pass.PassPath(); });
    pipeline.ExecuteStage(render::RenderStage::Denoise, context);
    Require(recovered == "Denoise/Recovered", "A failed frame must allow subsequent stage execution");

    Rejects<std::runtime_error>([&] {
        pipeline.ExecuteStage(render::RenderStage::Overlay, context, [](auto&) { throw std::runtime_error("Builtin failure"); });
    }, "Builtin setup exceptions must propagate");
    other.ExecuteStage(render::RenderStage::Overlay, context);
    Require(context.PassPath().empty(), "Builtin failures must also release context execution state");
}

// 资源黑板只发布稳定代理，缺失资源和未连接的 RHI 必须明确失败。
void ContextResourcesAndValidation() {
    render::RenderGraph graph;
    render::RenderSettings settings;
    render::RenderPipelineContext context(graph, 128, 96, settings);
    render::dx12::GpuResource first;
    render::dx12::GpuResource replacement;
    context.Publish("Scene.Color", first);
    Require(context.Find("Scene.Color") == &first && &context.Require("Scene.Color") == &first,
        "Published resources must preserve their stable external identity");
    context.Publish("Scene.Color", replacement);
    Require(context.Find("Scene.Color") == &replacement, "Later stages may publish a replacement logical output");
    Require(context.Find("Missing") == nullptr, "Optional resource lookup must represent missing values");
    Rejects<std::out_of_range>([&] { context.Require("Missing"); }, "Required resource lookup must reject missing values");
    Rejects<std::invalid_argument>([&] { context.Publish("", first); }, "Empty resource names must be rejected");
    Rejects<std::logic_error>([&] { context.Executor(); }, "CPU contexts must reject RHI executor access");
    Rejects<std::logic_error>([&] { context.Device(); }, "CPU contexts must reject device access");
    Rejects<std::logic_error>([&] { context.Descriptors(); }, "CPU contexts must reject descriptor access");
    Require(&context.settings == &settings, "Settings must be provided through the public immutable frame boundary");

    render::RenderPipeline pipeline;
    Rejects<std::invalid_argument>([&] { pipeline.GetStagePass(static_cast<render::RenderStage>(100)); },
        "Invalid stage values must be rejected");
    Rejects<std::invalid_argument>([&] { pipeline.RegisterPass(render::RenderStage::Prepare,
        static_cast<render::PassLocation>(100), "Invalid", [](auto&) {}); }, "Invalid location values must be rejected");
    Rejects<std::invalid_argument>([&] { pipeline.RegisterPass(render::RenderStage::Prepare,
        render::PassLocation::Before, "", [](auto&) {}); }, "Empty extension names must be rejected");
    Rejects<std::invalid_argument>([&] { pipeline.RegisterPass(render::RenderStage::Prepare,
        render::PassLocation::Before, "EmptySetup", {}); }, "Extension setup callbacks must be present");
}

// 外部模块卸载后，已构建的图仍持有回调数据；下一帧不再添加模块 Pass。
void ExternalModuleLifecycle() {
    render::RenderPipeline pipeline;
    render::RenderGraph graph;
    render::RenderSettings settings;
    render::RenderPipelineContext context(graph, 64, 64, settings);
    const auto executed = std::make_shared<std::vector<std::string>>();
    {
        RecordingExtension extension(executed);
        render::IRenderExtension& externalInterface = extension;
        externalInterface.RegisterPasses(pipeline);
        pipeline.ExecuteStage(render::RenderStage::BasePass, context);
        extension.UnregisterPasses(pipeline);
    }
    graph.Compile();
    graph.Execute([](const render::Barrier&) {});
    Require(*executed == std::vector<std::string>{"BasePass/External", "BasePass/External/Nested"},
        "Deferred graph callbacks must not reference a destroyed module registration object");
    render::RenderGraph nextGraph;
    render::RenderPipelineContext nextContext(nextGraph, 64, 64, settings);
    pipeline.ExecuteStage(render::RenderStage::BasePass, nextContext);
    Require(nextGraph.Compile().empty(), "Unloaded modules must leave no pass registrations in later frames");
}
}

int main() {
    try {
        // 所有用例仅验证建图及 CPU 执行协议，不创建窗口、设备或命令队列。
        StageAndSubpassOrder();
        CrossStageGraphExecution();
        HandlesAndUnregistration();
        ReentrancyAndExceptionRecovery();
        ContextResourcesAndValidation();
        ExternalModuleLifecycle();
        std::cout << "Render pipeline extension tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
