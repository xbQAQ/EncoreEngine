#pragma once
#include "dx12/Dx12Common.h"
#include <algorithm>
#include <array>

namespace render::dx12 {
// 资源参数同时驱动依赖声明和原生绑定，避免维护两份容易失配的列表。
class PassParameters {
public:
    PassParameters& Srv(unsigned slot, GpuResource& resource, D3D12_RESOURCE_STATES state = ShaderRead) {
        if (slot >= srvs_.size()) throw std::out_of_range("RDG SRV slot exceeds root signature");
        SetSlot(srvs_[slot], resource);
        return Access(resource, ResourceAccess::Read, state);
    }
    PassParameters& Uav(unsigned slot, GpuResource& resource, ResourceAccess access = ResourceAccess::Write) {
        if (slot >= uavs_.size() || access == ResourceAccess::Read) throw std::invalid_argument("RDG UAV binding requires write access");
        SetSlot(uavs_[slot], resource);
        return Access(resource, access, UavState());
    }
    PassParameters& Constants(GpuResource& resource) {
        SetSlot(constants_, resource);
        return Access(resource, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    PassParameters& Access(GpuResource& resource, ResourceAccess access, D3D12_RESOURCE_STATES state) {
        // 同一资源的多个只读视图可以合并状态；不同写入方式必须显式声明。
        const auto found = std::find_if(resources_.begin(), resources_.end(), [&](const Parameter& parameter) {
            return parameter.resource == &resource;
        });
        if (found == resources_.end()) resources_.push_back({&resource, access, state});
        else if (found->access == ResourceAccess::Read && access == ResourceAccess::Read)
            found->state = static_cast<D3D12_RESOURCE_STATES>(found->state | state);
        else if (found->state != state || found->access != access)
            throw std::invalid_argument("RDG conflicting views require a single explicit access mode");
        return *this;
    }
    std::vector<Use> Uses(const RenderGraph& graph) const {
        std::vector<Use> uses;
        for (const auto& parameter : resources_) {
            if (parameter.resource->graphOwner != &graph)
                throw std::invalid_argument("RDG parameter belongs to another graph or was not registered");
            uses.emplace_back(parameter.resource->graphId, static_cast<unsigned>(parameter.state), parameter.access);
        }
        return uses;
    }
    PassId AddPass(RenderGraph& graph, std::string name,
        std::function<void(const PassParameters&)> execute, bool sideEffect = false) const {
        if (!execute) throw std::invalid_argument("RDG parameter pass requires a callback");
        auto uses = Uses(graph);
        // 参数按值归图持有，建图栈上的临时参数不会逃逸到录制阶段。
        return graph.AddPass(std::move(name), std::move(uses), [parameters = *this, &graph, execute = std::move(execute)] {
            parameters.Validate(graph);
            execute(parameters);
        }, sideEffect);
    }
    void Validate(const RenderGraph& graph) const {
        for (const auto& parameter : resources_) {
            if (parameter.resource->graphOwner != &graph)
                throw std::logic_error("RDG parameter graph identity changed before execution");
            graph.AssertAccess(parameter.resource->graphId, parameter.access, static_cast<unsigned>(parameter.state));
            if (!parameter.resource->object) throw std::logic_error("RDG declared resource is not resident during its pass");
        }
    }
    const std::array<GpuResource*, 32>& Srvs() const { return srvs_; }
    const std::array<GpuResource*, 8>& Uavs() const { return uavs_; }
    GpuResource* ConstantBuffer() const { return constants_; }
private:
    struct Parameter {
        GpuResource* resource{}; // 图注册的稳定逻辑代理，不在建图阶段解析原生对象。
        ResourceAccess access{ResourceAccess::Read}; // 内容读取、完整覆盖或依赖旧值的原地更新。
        D3D12_RESOURCE_STATES state{}; // 该参数在命令录制期间要求的资源状态。
    };
    static constexpr D3D12_RESOURCE_STATES UavState() { return D3D12_RESOURCE_STATE_UNORDERED_ACCESS; }
    static void SetSlot(GpuResource*& slot, GpuResource& resource) {
        if (slot && slot != &resource) throw std::invalid_argument("RDG shader parameter slot was bound twice");
        slot = &resource;
    }
    std::array<GpuResource*, 32> srvs_{}; // 根签名 t0..t31，空槽明确表示着色器不访问该输入。
    std::array<GpuResource*, 8> uavs_{}; // 根签名 u0..u7，仅为本 Pass 输出生成视图。
    GpuResource* constants_{}; // 根 CBV b0，同样参与图依赖及执行期访问校验。
    std::vector<Parameter> resources_; // 由绑定参数合并得到的唯一资源访问集。
};
}
