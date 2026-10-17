#include "dx12/DescriptorArena.h"

namespace render::dx12 {
namespace {
void CheckIndex(unsigned index, unsigned count, const char* what) {
    if (index >= count) throw std::out_of_range(std::string(what) + " descriptor index is out of bounds");
}
void CheckRange(unsigned first, unsigned count, unsigned limit, const char* what) {
    if (count == 0 || first >= limit || count > limit - first)
        throw std::out_of_range(std::string(what) + " descriptor range is out of bounds");
}
}

DescriptorArena::DescriptorArena(Dx12Device& device) : device_(device) {
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = HeapSize;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(device_.Native()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&cpuHeap_)), "CPU descriptor heap");
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(device_.Native()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&shaderHeap_)), "Shader descriptor heap");
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    description.NumDescriptors = RtvCount;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(device_.Native()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&rtvHeap_)), "RTV descriptor heap");
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    description.NumDescriptors = 2;
    Check(device_.Native()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&dsvHeap_)), "DSV descriptor heap");
    descriptorSize_ = device_.Native()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvSize_ = device_.Native()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvSize_ = device_.Native()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // 槽位 0 保留给 ImGui 字体，1 为视口，2 为 TLAS，3 为带类型的空 SRV。
    D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
    nullView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullView.Texture2D.MipLevels = 1;
    device_.Native()->CreateShaderResourceView(nullptr, &nullView, Cpu(3));
    device_.Native()->CopyDescriptorsSimple(1, VisibleCpu(3), Cpu(3), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // 着色器未使用共享 UAV 槽位时，由图拥有的输出可能尚未驻留。
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullWritable{};
    nullWritable.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    nullWritable.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_.Native()->CreateUnorderedAccessView(nullptr, nullptr, &nullWritable, Cpu(4));
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorArena::Cpu(unsigned index) const {
    CheckIndex(index, HeapSize, "CPU");
    auto handle = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
    return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorArena::VisibleCpu(unsigned index) const {
    CheckIndex(index, HeapSize, "visible CPU");
    auto handle = shaderHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
    return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE DescriptorArena::Gpu(unsigned index) const {
    CheckIndex(index, HeapSize, "GPU");
    auto handle = shaderHeap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * descriptorSize_;
    return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorArena::Rtv(unsigned index) const {
    CheckIndex(index, RtvCount, "RTV");
    auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * rtvSize_;
    return handle;
}

unsigned DescriptorArena::AllocatePersistent(unsigned count) {
    CheckRange(nextPersistent_, count, TransientStart, "persistent");
    const unsigned result = nextPersistent_;
    nextPersistent_ += count;
    return result;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorArena::Dsv(unsigned index) const {
    // 虽然按顺序录制，相机与阴影深度仍需要独立描述符。
    CheckIndex(index, 2, "DSV");
    auto handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * dsvSize_;
    return handle;
}

unsigned DescriptorArena::AllocateTable(unsigned count) {
    CheckRange(nextTransient_, count, HeapSize, "transient");
    const unsigned result = nextTransient_;
    nextTransient_ += count;
    return result;
}

void DescriptorArena::ResetFrame() { nextTransient_ = TransientStart; }

void DescriptorArena::CopyToVisible(unsigned destination, unsigned source) {
    CheckIndex(destination, HeapSize, "visible destination");
    CheckIndex(source, HeapSize, "CPU source");
    device_.Native()->CopyDescriptorsSimple(1, VisibleCpu(destination), Cpu(source), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void DescriptorArena::Bind(ID3D12GraphicsCommandList* commands) const {
    if (!commands) throw std::invalid_argument("Cannot bind descriptor arena to a null command list");
    ID3D12DescriptorHeap* heaps[]{shaderHeap_.Get()};
    commands->SetDescriptorHeaps(1, heaps);
}
}
