#pragma once

#include "dx12/Dx12Device.h"

namespace render::dx12 {

class DescriptorArena {
public:
    // Allocate CPU source, shader-visible, RTV and DSV heaps for one device.
    explicit DescriptorArena(Dx12Device& device);

    // Copying an allocation cursor would let two arenas overwrite the same descriptor ranges.
    DescriptorArena(const DescriptorArena&) = delete;
    DescriptorArena& operator=(const DescriptorArena&) = delete;

    ID3D12DescriptorHeap* ShaderHeap() const { return shaderHeap_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE VisibleCpu(unsigned index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(unsigned index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(unsigned index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(unsigned index = 0) const;

    // Persistent views occupy [32, 1024), after ImGui and reserved null descriptors.
    unsigned AllocatePersistent(unsigned count = 1);
    // Per-pass tables occupy [1024, 8192) and are reset at the start of each frame.
    unsigned AllocateTable(unsigned count);
    void ResetFrame();
    // Scene replacement releases all pool resources after the queue is idle, retaining editor slots.
    void ResetPersistent() { nextPersistent_ = PersistentStart; ResetFrame(); }
    // Copy a CPU-only descriptor source into the shader-visible heap at destination.
    void CopyToVisible(unsigned destination, unsigned source);
    // Bind the shader-visible descriptor heap to a graphics or compute list.
    void Bind(ID3D12GraphicsCommandList* commands) const;

private:
    static constexpr unsigned HeapSize = 8192; // Shared CBV/SRV/UAV heap capacity.
    static constexpr unsigned PersistentStart = 32; // Slots below this are reserved for editor views.
    static constexpr unsigned TransientStart = 1024; // Per-frame table arena start.
    static constexpr unsigned RtvCount = 16; // Swapchain and render target view capacity.

    Dx12Device& device_; // Device used to create heaps and copy descriptors.
    ComPtr<ID3D12DescriptorHeap> cpuHeap_; // Non-visible source descriptors for stable resources.
    ComPtr<ID3D12DescriptorHeap> shaderHeap_; // Shader-visible descriptors bound during passes.
    ComPtr<ID3D12DescriptorHeap> rtvHeap_; // CPU-only RTV descriptors.
    ComPtr<ID3D12DescriptorHeap> dsvHeap_; // CPU-only depth views for the camera and shadow atlas.
    unsigned descriptorSize_{}; // Device-specific CBV/SRV/UAV descriptor stride.
    unsigned rtvSize_{}; // Device-specific RTV descriptor stride.
    unsigned dsvSize_{}; // Device-specific DSV descriptor stride.
    unsigned nextPersistent_{PersistentStart}; // Next unused persistent CBV/SRV/UAV slot.
    unsigned nextTransient_{TransientStart}; // Next unused transient table slot.
};

}
