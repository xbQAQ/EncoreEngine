#include "dx12/Dx12Swapchain.h"

#include <algorithm>
#include <iostream>

namespace render::dx12 {

Dx12Swapchain::Dx12Swapchain(Dx12Device& device, DescriptorArena& descriptors, HWND window)
    : device_(device), descriptors_(descriptors), window_(window) {
    if (!window_) throw std::invalid_argument("Cannot create a swapchain for a null window");
    RECT client{};
    if (!GetClientRect(window_, &client)) throw std::runtime_error("GetClientRect failed");
    width_ = std::max(1L, client.right - client.left);
    height_ = std::max(1L, client.bottom - client.top);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width_;
    description.Height = height_;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;
    Check(device_.Factory()->CreateSwapChainForHwnd(device_.Queue(), window_, &description,
        nullptr, nullptr, &base), "Swapchain");
    Check(base.As(&swapchain_), "Swapchain3");
    Check(device_.Factory()->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER), "Window association");
    CreateBackbuffers();
}

Dx12Swapchain::~Dx12Swapchain() {
    // Present may still reference these images after the frame fence; retire it before final-release of backbuffers.
    try { device_.WaitIdle(); }
    catch (const std::exception& error) { std::cerr << "Swapchain retirement: " << error.what() << '\n'; }
}

void Dx12Swapchain::CreateBackbuffers() {
    for (unsigned index = 0; index < backbuffers_.size(); ++index) {
        Check(swapchain_->GetBuffer(index, IID_PPV_ARGS(&backbuffers_[index].object)), "Swapchain backbuffer");
        backbuffers_[index].state = D3D12_RESOURCE_STATE_PRESENT;
        backbuffers_[index].name = "Backbuffer" + std::to_string(index);
        backbuffers_[index].object->SetName(std::wstring(backbuffers_[index].name.begin(), backbuffers_[index].name.end()).c_str());
        device_.Native()->CreateRenderTargetView(backbuffers_[index].object.Get(), nullptr, descriptors_.Rtv(index));
    }
}

void Dx12Swapchain::Resize(unsigned width, unsigned height) {
    if (!width || !height || (width == width_ && height == height_)) return;
    // ResizeBuffers requires every outstanding reference and GPU use to be finished first.
    device_.WaitIdle();
    for (auto& backbuffer : backbuffers_) backbuffer.object.Reset();
    Check(swapchain_->ResizeBuffers(static_cast<UINT>(backbuffers_.size()), width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0), "Resize swapchain buffers");
    width_ = width;
    height_ = height;
    CreateBackbuffers();
}

unsigned Dx12Swapchain::CurrentIndex() const { return swapchain_->GetCurrentBackBufferIndex(); }
GpuResource& Dx12Swapchain::Current() { return backbuffers_[CurrentIndex()]; }
void Dx12Swapchain::Present(bool vsync) { Check(swapchain_->Present(vsync ? 1 : 0, 0), "Present"); }
}
