#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace render {
using ResourceId = std::uint32_t;
using PassId = std::uint32_t;
// 保持 CPU 图不依赖 D3D 头文件，同时匹配 D3D12 的无序访问状态值。
inline constexpr std::uint32_t UnorderedAccessState = 0x8;
inline constexpr std::uint32_t InvalidGraphIndex = std::numeric_limits<std::uint32_t>::max();

enum class ResourceKind {
    Buffer, // 默认堆中的线性结构化或原始缓冲区。
    Texture2D, // 单采样、单 mip 的二维图像。
};

enum class ResourceMemory {
    Default, // 供 GPU 计算、渲染和复制使用的默认堆。
    Upload, // CPU 写入的上传堆，同次 GPU 提交内不能按 Pass 寿命复用字节。
};

struct ResourceDescriptor {
    ResourceKind kind{ResourceKind::Texture2D}; // 原生物理分配类型。
    std::uint64_t width{}; // 缓冲区字节数或纹理的像素宽度。
    std::uint32_t height{1}; // 纹理的像素高度；缓冲区固定为一。
    std::uint32_t format{}; // 后端格式编号；结构化缓冲区使用零。
    std::uint32_t stride{}; // 结构化缓冲区元素字节数；纹理使用零。
    std::uint32_t flags{}; // 后端物理分配的能力标记。
    std::array<float, 4> clear{}; // 优化清除值，也是物理分配兼容性的一部分。
    ResourceMemory memory{ResourceMemory::Default}; // 物理堆类型，上传常量需保持提交期内容稳定。
    bool operator==(const ResourceDescriptor&) const = default;
};

struct ResourceLifetime {
    std::uint32_t firstPass{InvalidGraphIndex}; // 保留执行计划中首次需要存储的位置。
    std::uint32_t lastPass{InvalidGraphIndex}; // 末次使用位置；计划长度表示图导出完成阶段。
    std::uint32_t allocation{InvalidGraphIndex}; // 兼容物理槽位；导入或被裁掉的资源使用无效编号。
};

enum class ResourceAccess {
    Read, // 只消费此前已初始化的内容，不改变资源值。
    Write, // 完整覆盖资源内容，不消费此前的逻辑值。
    ReadWrite, // 原地修改内容，必须保留此前生产者并验证初始化。
};

// 读写属性共同驱动值依赖、初始化校验和原生资源访问授权。
inline constexpr bool Reads(ResourceAccess access) {
    return access == ResourceAccess::Read || access == ResourceAccess::ReadWrite;
}

// 完整覆盖与原地修改都会生产新值，但只有后者消费旧值。
inline constexpr bool Writes(ResourceAccess access) {
    return access == ResourceAccess::Write || access == ResourceAccess::ReadWrite;
}

struct Use {
    ResourceId resource{}; // 由当前图的 Import 或 Create 声明的逻辑资源。
    std::uint32_t state{}; // 执行此 Pass 时要求的后端资源状态。
    ResourceAccess access{ResourceAccess::Read}; // 区分消费旧值、完整覆盖和原地读写。
    Use() = default;
    Use(ResourceId id, std::uint32_t requiredState, ResourceAccess mode) : resource(id), state(requiredState), access(mode) {}
    // 保留旧调用的初始化写法；新接口应明确区分完整覆盖和原地读写。
    Use(ResourceId id, std::uint32_t requiredState, bool write)
        : Use(id, requiredState, write ? ResourceAccess::Write : ResourceAccess::Read) {}
};

struct Barrier {
    ResourceId resource{}; // 下一操作之前需要同步的资源。
    std::uint32_t before{}; // 此前已建立的后端状态。
    std::uint32_t after{}; // 需要的后端状态；UAV 顺序屏障与此前状态相同。
    bool uav{}; // 为真表示同状态写入后的顺序同步，而非状态转换。
};

struct CompiledPass {
    std::string name; // 用于图检查和 GPU 事件的可读 Pass 名称。
    PassId id{}; // 原始声明编号，在裁剪和调度后仍保留。
    std::vector<Barrier> barriers; // 紧邻此 Pass 之前执行的同步命令。
    std::vector<ResourceId> acquire; // 在 Pass 屏障之前物化的瞬时分配。
    std::vector<ResourceId> release; // 录制此 Pass 后归还的瞬时分配。
    std::vector<Use> uses; // 与执行期资源解析共同使用的唯一访问声明。
};

struct GraphResource {
    std::string name; // 用于诊断和图检查的可读资源名称。
    std::uint32_t initialState{}; // 图执行开始时的后端状态。
    bool initialized{}; // 导入内容是否允许在图内首次写入之前读取。
    bool transient{}; // 为真表示物理分配与寿命由当前图管理。
    ResourceDescriptor description{}; // 由图拥有的存储对应的物理创建要求。
    bool preserveWrites{}; // 外部可观察资源的写入即使未导出也必须执行。
};

class RenderGraph {
public:
    ResourceId Import(std::string name, std::uint32_t initialState, bool initialized = true, bool preserveWrites = true);
    ResourceId Create(std::string name, const ResourceDescriptor& description);
    PassId AddPass(std::string name, std::vector<Use> uses, std::function<void()> execute, bool sideEffect = false);
    // 消费者存活时同时保留指定前置工作，适用于资源声明之外的生产关系。
    void AddDependency(PassId before, PassId after);
    // 只约束已保留 Pass 的执行顺序，不会因后继存活而保活前驱。
    void AddOrdering(PassId before, PassId after);
    void Export(ResourceId resource, std::uint32_t finalState);
    const std::vector<CompiledPass>& Compile();
    void Execute(const std::function<void(const Barrier&)>& transition,
        const std::function<void(ResourceId, bool)>& lifetime = {},
        const std::function<void(const CompiledPass&)>& beforePass = {},
        const std::function<void(const CompiledPass&)>& afterPass = {});
    void AssertAccess(ResourceId resource, ResourceAccess access, std::uint32_t requiredState) const;
    const std::vector<CompiledPass>& Plan() const { return plan_; }
    const std::vector<GraphResource>& Resources() const { return resources_; }
    const std::vector<Barrier>& FinalBarriers() const { return finalBarriers_; }
    const std::vector<ResourceLifetime>& Lifetimes() const { return lifetimes_; }
    const std::vector<ResourceDescriptor>& Allocations() const { return allocations_; }

private:
    void AssertMutable() const;
    struct Pass {
        std::string name; // 编译计划和诊断中保留的调试名称。
        std::vector<Use> uses; // 用于推导访问冲突的资源访问声明。
        std::function<void()> execute; // 只有此 Pass 未被裁掉时才调用的录制回调。
        bool sideEffect{}; // 导出资源之外可观察工作的显式存活根。
    };
    struct ExportedResource {
        ResourceId resource{}; // 最终值可由图外部观察的资源。
        std::uint32_t state{}; // 图执行完成后要求的后端状态。
    };
    std::vector<GraphResource> resources_; // 按 ResourceId 索引的导入资源与图创建资源。
    std::vector<Pass> passes_; // 按 PassId 索引的原始 Pass 声明。
    std::vector<std::pair<PassId, PassId>> dependencies_; // 显式生产者到消费者依赖边。
    std::vector<std::pair<PassId, PassId>> orderings_; // 仅参与拓扑排序和周期校验，不传播保活的显式边。
    std::vector<ExportedResource> exports_; // 最终资源状态和存活根。
    std::vector<CompiledPass> plan_; // 验证和裁剪之后的稳定拓扑执行顺序。
    std::vector<Barrier> finalBarriers_; // 最后一个保留 Pass 之后执行的状态转换。
    std::vector<ResourceLifetime> lifetimes_; // 保留执行计划中的寿命区间，不依赖原始声明顺序。
    std::vector<ResourceDescriptor> allocations_; // 仅供兼容且寿命不重叠的资源共享的物理槽位。
    bool compiled_{}; // 每次建图操作都会使其失效，防止执行过期计划。
    bool executing_{}; // 整次执行期间禁止重入或修改已编译的资源和 Pass。
    PassId activePass_{InvalidGraphIndex}; // 仅执行回调录制期间授予对应 Pass 的访问权限。
};
}
