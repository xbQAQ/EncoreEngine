#pragma once
// 管线缓存消费材质编译结果，编译实现由材质层独立维护。
#include "dx12/Dx12Common.h"
#include "render/MaterialScripts.h"
#include "Materials/MaterialShaderCompiler.h"
#include <map>
#include <span>
#include <future>
#include <optional>

namespace render::dx12 { class Dx12Device; }
namespace render {
// 管线创建独立于场景资源所有权和逐帧图构建。
class PipelineCache {
public:
    explicit PipelineCache(dx12::Dx12Device&);
    ID3D12RootSignature* Root() const { return root_.Get(); }
    ID3D12PipelineState* GBuffer() const { return gbuffer_.Get(); }
    ID3D12PipelineState* Shadow() const { return shadow_.Get(); }
    ID3D12PipelineState* Compute(const std::wstring& name, unsigned backend) const;
    MaterialCompileResult ReloadMaterials(std::span<const MaterialShaderSource> sources);
    void RequestMaterialReload(std::span<const MaterialShaderSource> sources);
    std::optional<MaterialCompileResult> PollMaterialReload();
    bool MaterialCompilationPending() const { return compilation_.valid() || queued_.has_value(); }
    std::uint64_t MaterialRevision() const { return materialRevision_; }
private:
    MaterialCompileResult PublishMaterials(MaterialShaderBundle candidate);
    void StartMaterialCompilation(std::vector<MaterialShaderSource> sources);
    dx12::Dx12Device& device_; // 借用渲染器的设备进行队列同步及原生管线创建。
    dx12::ComPtr<ID3D12RootSignature> root_; // 共享 b0、b1、t0 至 t31、u0 至 u7 的绑定约定。
    dx12::ComPtr<ID3D12PipelineState> gbuffer_; // 硬件光栅化 GBuffer 管线。
    dx12::ComPtr<ID3D12PipelineState> shadow_; // 输出线性深度和矩阴影的光栅化管线。
    std::map<std::wstring, dx12::ComPtr<ID3D12PipelineState>> compute_; // 按算法和后端查找计算管线。
    std::uint64_t materialRevision_{}; // 仅在完整候选管线集合发布后递增。
    std::future<MaterialShaderBundle> compilation_; // 工作线程独占源码副本并生成 DXIL，不访问 D3D12。
    std::optional<std::vector<MaterialShaderSource>> queued_; // 编译进行时只保留最新编辑，覆盖中间草稿。
};
}
