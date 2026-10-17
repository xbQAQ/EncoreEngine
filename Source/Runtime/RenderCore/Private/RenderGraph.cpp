#include "render/RenderGraph.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>

namespace render {
void RenderGraph::AssertMutable() const {
    // 防止录制回调或生命周期钩子使迭代中的计划、资源编号和访问授权失效。
    if (executing_) throw std::logic_error("render graph cannot be modified during execution");
}

ResourceId RenderGraph::Import(std::string name, std::uint32_t initialState, bool initialized, bool preserveWrites) {
    AssertMutable();
    if (resources_.size() >= std::numeric_limits<ResourceId>::max()) throw std::length_error("render graph resource limit exceeded");
    compiled_ = false;
    const auto id = static_cast<ResourceId>(resources_.size());
    resources_.push_back({std::move(name), initialState, initialized, false, {}, preserveWrites});
    return id;
}

ResourceId RenderGraph::Create(std::string name, const ResourceDescriptor& description) {
    AssertMutable();
    // 即使复用物理分配，池中残留字节也不能初始化新的逻辑值。
    if (!description.width || !description.height ||
        (description.memory != ResourceMemory::Default && description.memory != ResourceMemory::Upload) ||
        (description.memory == ResourceMemory::Upload &&
            (description.kind != ResourceKind::Buffer || description.flags || description.stride)) ||
        (description.kind == ResourceKind::Buffer && (description.height != 1 ||
            (description.stride && description.width % description.stride != 0))) ||
        (description.kind == ResourceKind::Texture2D && (!description.format || description.stride)))
        throw std::invalid_argument("render graph transient descriptor is invalid");
    const auto id = Import(std::move(name), 0, false, false);
    resources_[id].transient = true;
    resources_[id].description = description;
    // 上传堆的状态由 D3D12 固定为 GENERIC_READ，不能过渡到 COMMON 或 COPY_DEST。
    if (description.memory == ResourceMemory::Upload) resources_[id].initialState = 0xac3;
    return id;
}

PassId RenderGraph::AddPass(std::string name, std::vector<Use> uses, std::function<void()> execute, bool sideEffect) {
    AssertMutable();
    if (passes_.size() >= std::numeric_limits<PassId>::max()) throw std::length_error("render graph pass limit exceeded");
    compiled_ = false;
    const auto id = static_cast<PassId>(passes_.size());
    passes_.push_back({std::move(name), std::move(uses), std::move(execute), sideEffect});
    return id;
}

void RenderGraph::AddDependency(PassId before, PassId after) {
    AssertMutable();
    compiled_ = false;
    dependencies_.emplace_back(before, after);
}

void RenderGraph::AddOrdering(PassId before, PassId after) {
    // 最终显示等边界只需要排序，不能把原本可裁剪的算法分支变成存活根。
    AssertMutable();
    compiled_ = false;
    orderings_.emplace_back(before, after);
}

void RenderGraph::Export(ResourceId resource, std::uint32_t finalState) {
    AssertMutable();
    compiled_ = false;
    exports_.push_back({resource, finalState});
}

const std::vector<CompiledPass>& RenderGraph::Compile() {
    AssertMutable();
    compiled_ = false;
    plan_.clear();
    finalBarriers_.clear();
    lifetimes_.assign(resources_.size(), {});
    allocations_.clear();
    const auto passCount = passes_.size();
    constexpr PassId noPass = std::numeric_limits<PassId>::max();
    std::vector<std::set<PassId>> outgoing(passCount);
    std::vector<std::set<PassId>> incoming(passCount);
    std::vector<std::set<PassId>> valueDependencies(passCount);
    std::vector<PassId> lastWriter(resources_.size(), noPass);
    std::vector<std::vector<PassId>> readers(resources_.size());
    std::vector<bool> initialized;
    initialized.reserve(resources_.size());
    for (const auto& resource : resources_) initialized.push_back(resource.initialized);
    const auto addEdge = [&](PassId before, PassId after) {
        outgoing[before].insert(after);
        incoming[after].insert(before);
    };
    const auto addValueDependency = [&](PassId before, PassId after) {
        addEdge(before, after);
        valueDependencies[after].insert(before);
    };

    // 在裁剪之前验证全部声明，避免无用 Pass 隐藏非法图代码。
    for (PassId pass = 0; pass < passCount; ++pass) {
        const auto& declaration = passes_[pass];
        if (!declaration.execute) throw std::invalid_argument("render graph pass '" + declaration.name + "' has no callback");
        std::set<ResourceId> seen;
        for (const auto& use : declaration.uses) {
            if (use.resource >= resources_.size())
                throw std::invalid_argument("render graph pass '" + declaration.name + "' uses an invalid resource");
            const auto& resource = resources_[use.resource];
            if (!seen.insert(use.resource).second)
                throw std::invalid_argument("render graph pass '" + declaration.name + "' declares resource '" + resource.name + "' more than once");
            if (use.access != ResourceAccess::Read && use.access != ResourceAccess::Write && use.access != ResourceAccess::ReadWrite)
                throw std::invalid_argument("render graph pass '" + declaration.name + "' declares an invalid access mode");
            if (Reads(use.access) && !initialized[use.resource])
                throw std::invalid_argument("render graph pass '" + declaration.name + "' reads uninitialized resource '" + resource.name + "'");
            if (lastWriter[use.resource] != noPass) {
                // RAW 与原地读写消费旧值；完整覆盖只需要排序，不能据此保活旧值。
                if (Reads(use.access)) addValueDependency(lastWriter[use.resource], pass);
                else addEdge(lastWriter[use.resource], pass);
            }
            if (Writes(use.access)) {
                // 原地覆盖之前必须完成尚未结束的读取，避免 WAR 冲突。
                for (const auto reader : readers[use.resource]) addEdge(reader, pass);
                readers[use.resource].clear();
                lastWriter[use.resource] = pass;
                initialized[use.resource] = true;
            } else {
                readers[use.resource].push_back(pass);
            }
        }
    }
    for (const auto& [before, after] : dependencies_) {
        if (before >= passCount || after >= passCount) throw std::invalid_argument("render graph explicit dependency has an invalid pass");
        // 显式依赖表达调用方要求执行的前置工作，而非仅资源复用造成的排序。
        addValueDependency(before, after);
    }
    for (const auto& [before, after] : orderings_) {
        if (before >= passCount || after >= passCount) throw std::invalid_argument("render graph explicit ordering has an invalid pass");
        // 顺序边参与全图环检查，但不进入消费者向生产者传播存活的依赖集合。
        addEdge(before, after);
    }
    std::set<ResourceId> exported;
    for (const auto& output : exports_) {
        if (output.resource >= resources_.size()) throw std::invalid_argument("render graph exports an invalid resource");
        if (!exported.insert(output.resource).second) throw std::invalid_argument("render graph exports a resource more than once");
        if (!initialized[output.resource])
            throw std::invalid_argument("render graph exports uninitialized resource '" + resources_[output.resource].name + "'");
    }

    // 在依赖允许时，按最小编号出队以保留声明顺序。
    std::vector<std::size_t> remaining(passCount);
    std::priority_queue<PassId, std::vector<PassId>, std::greater<PassId>> ready;
    for (PassId pass = 0; pass < passCount; ++pass) {
        remaining[pass] = incoming[pass].size();
        if (remaining[pass] == 0) ready.push(pass);
    }
    std::vector<PassId> topological;
    topological.reserve(passCount);
    while (!ready.empty()) {
        const auto pass = ready.top();
        ready.pop();
        topological.push_back(pass);
        for (const auto next : outgoing[pass]) if (--remaining[next] == 0) ready.push(next);
    }
    if (topological.size() != passCount) throw std::invalid_argument("render graph contains a dependency cycle");

    std::vector<bool> live(passCount, false);
    std::vector<PassId> pending;
    for (PassId pass = 0; pass < passCount; ++pass) {
        // 导入的可观察写入是副作用根，图内瞬时完整覆盖则允许裁掉旧生产者。
        if (passes_[pass].sideEffect || std::any_of(passes_[pass].uses.begin(), passes_[pass].uses.end(), [&](const Use& use) {
            return Writes(use.access) && resources_[use.resource].preserveWrites;
        })) pending.push_back(pass);
    }
    for (const auto& output : exports_) if (lastWriter[output.resource] != noPass) pending.push_back(lastWriter[output.resource]);
    while (!pending.empty()) {
        const auto pass = pending.back();
        pending.pop_back();
        if (live[pass]) continue;
        live[pass] = true;
        for (const auto dependency : valueDependencies[pass]) pending.push_back(dependency);
    }

    std::vector<std::uint32_t> states;
    std::vector<bool> accessed;
    std::vector<bool> previousWrite;
    for (const auto& resource : resources_) {
        states.push_back(resource.initialState);
        accessed.push_back(resource.initialized);
        previousWrite.push_back(resource.initialized && resource.initialState == UnorderedAccessState);
    }
    for (const auto pass : topological) {
        if (!live[pass]) continue;
        CompiledPass compiled{passes_[pass].name, pass, {}, {}, {}, passes_[pass].uses};
        for (const auto& use : passes_[pass].uses) {
            const auto resource = use.resource;
            auto& lifetime = lifetimes_[resource];
            if (lifetime.firstPass == InvalidGraphIndex) lifetime.firstPass = static_cast<std::uint32_t>(plan_.size());
            lifetime.lastPass = static_cast<std::uint32_t>(plan_.size());
            if (states[resource] != use.state) {
                compiled.barriers.push_back({resource, states[resource], use.state, false});
            } else if (use.state == UnorderedAccessState && accessed[resource] && (previousWrite[resource] || Writes(use.access))) {
                // 即使状态不变，UAV 访问冲突仍需要顺序同步。
                compiled.barriers.push_back({resource, use.state, use.state, true});
            }
            states[resource] = use.state;
            accessed[resource] = true;
            previousWrite[resource] = Writes(use.access);
        }
        plan_.push_back(std::move(compiled));
    }
    for (const auto& output : exports_) {
        // 导出资源保留物理分配，直到最终状态转换完成。
        if (resources_[output.resource].transient)
            lifetimes_[output.resource].lastPass = static_cast<std::uint32_t>(plan_.size());
        if (states[output.resource] != output.state)
            finalBarriers_.push_back({output.resource, states[output.resource], output.state, false});
        else if (output.state == UnorderedAccessState && previousWrite[output.resource])
            finalBarriers_.push_back({output.resource, output.state, output.state, true});
    }
    // 按区间贪心复用完整资源，不对 placed heap 内存执行别名复用。
    std::vector<ResourceId> temporaries;
    for (ResourceId id = 0; id < resources_.size(); ++id)
        if (resources_[id].transient && lifetimes_[id].firstPass != InvalidGraphIndex) temporaries.push_back(id);
    std::stable_sort(temporaries.begin(), temporaries.end(), [&](ResourceId a, ResourceId b) {
        return lifetimes_[a].firstPass < lifetimes_[b].firstPass;
    });
    std::vector<std::uint32_t> occupiedUntil;
    for (const auto id : temporaries) {
        auto& lifetime = lifetimes_[id];
        std::uint32_t slot = 0;
        for (; slot < allocations_.size(); ++slot)
            // CPU 在录制期间写上传堆；即使 GPU Pass 寿命不重叠，也必须保留各自的数据直到提交完成。
            if (resources_[id].description.memory != ResourceMemory::Upload &&
                occupiedUntil[slot] < lifetime.firstPass && allocations_[slot] == resources_[id].description) break;
        if (slot == allocations_.size()) {
            allocations_.push_back(resources_[id].description);
            occupiedUntil.push_back(lifetime.lastPass);
        } else occupiedUntil[slot] = lifetime.lastPass;
        lifetime.allocation = slot;
        plan_[lifetime.firstPass].acquire.push_back(id);
        if (lifetime.lastPass < plan_.size()) plan_[lifetime.lastPass].release.push_back(id);
    }
    compiled_ = true;
    return plan_;
}

void RenderGraph::Execute(const std::function<void(const Barrier&)>& transition,
    const std::function<void(ResourceId, bool)>& lifetime,
    const std::function<void(const CompiledPass&)>& beforePass,
    const std::function<void(const CompiledPass&)>& afterPass) {
    if (executing_) throw std::logic_error("render graph execution cannot reenter itself");
    if (!compiled_) throw std::logic_error("render graph must compile successfully after its last modification");
    if (!transition) throw std::invalid_argument("render graph requires a barrier callback");
    // 资源状态转换和 GPU 录制遵循同一计划，只有真正的 Pass 回调能解析原生资源。
    executing_ = true;
    try {
        for (const auto& pass : plan_) {
            if (beforePass) beforePass(pass);
            if (lifetime) for (const auto id : pass.acquire) lifetime(id, true);
            for (const auto& barrier : pass.barriers) transition(barrier);
            activePass_ = pass.id;
            passes_[pass.id].execute();
            activePass_ = InvalidGraphIndex;
            if (afterPass) afterPass(pass);
            if (lifetime) for (const auto id : pass.release) lifetime(id, false);
        }
        for (const auto& barrier : finalBarriers_) transition(barrier);
        if (lifetime) for (ResourceId id = 0; id < resources_.size(); ++id)
            if (resources_[id].transient && lifetimes_[id].firstPass != InvalidGraphIndex && lifetimes_[id].lastPass == plan_.size())
                lifetime(id, false);
    } catch (...) {
        // 录制失败不能把访问授权或禁止修改状态泄露到后续帧。
        activePass_ = InvalidGraphIndex;
        executing_ = false;
        throw;
    }
    executing_ = false;
}

void RenderGraph::AssertAccess(ResourceId resource, ResourceAccess access, std::uint32_t requiredState) const {
    // 声明是原生绑定的唯一授权来源，资源恰好驻留也不能绕过当前 Pass 的访问契约。
    if (!executing_ || activePass_ == InvalidGraphIndex)
        throw std::logic_error("render graph native resources are accessible only inside an executing pass");
    if (access != ResourceAccess::Read && access != ResourceAccess::Write && access != ResourceAccess::ReadWrite)
        throw std::invalid_argument("render graph native resource access mode is invalid");
    const auto& pass = passes_.at(activePass_);
    const auto declaration = std::find_if(pass.uses.begin(), pass.uses.end(), [&](const Use& use) { return use.resource == resource; });
    if (declaration == pass.uses.end())
        throw std::logic_error("render graph pass '" + pass.name + "' resolves an undeclared resource");
    if ((Reads(access) && !Reads(declaration->access)) || (Writes(access) && !Writes(declaration->access)))
        throw std::logic_error("render graph pass '" + pass.name + "' resolves an incompatible resource access");
    // 组合只读状态允许按位包含；COMMON 为零，必须显式相等而不能充当通配符。
    const bool compatibleState = requiredState ? (declaration->state & requiredState) == requiredState : declaration->state == 0;
    if (!compatibleState)
        throw std::logic_error("render graph pass '" + pass.name + "' resolves a resource in an undeclared state");
}
}
