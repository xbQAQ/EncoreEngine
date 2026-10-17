#pragma once

#include "dx12/DescriptorArena.h"
#include <array>

namespace render::dx12 {

class Dx12Swapchain {
public:
    // Create a two-buffer flip-discard swapchain sized from the window client rectangle.
    Dx12Swapchain(Dx12Device& device, DescriptorArena& descriptors, HWND window);
    ~Dx12Swapchain();

    // DXGI resize must have one owner for every outstanding backbuffer reference.
    Dx12Swapchain(const Dx12Swapchain&) = delete;
    Dx12Swapchain& operator=(const Dx12Swapchain&) = delete;

    void Resize(unsigned width, unsigned height);
    unsigned Width() const { return width_; }
    unsigned Height() const { return height_; }
    unsigned CurrentIndex() const;
    GpuResource& Current();
    void Present(bool vsync);

private:
    void CreateBackbuffers();

    Dx12Device& device_; // Device owning queue, factory and synchronization.
    DescriptorArena& descriptors_; // RTV heap owner used for backbuffer views.
    HWND window_{}; // Native window associated with the DXGI swapchain.
    ComPtr<IDXGISwapChain3> swapchain_; // Flip-discard two-buffer presentation chain.
    std::array<GpuResource, 2> backbuffers_{}; // Imported graph resources for each swapchain image.
    unsigned width_{}; // Current swapchain width in pixels.
    unsigned height_{}; // Current swapchain height in pixels.
};

}
