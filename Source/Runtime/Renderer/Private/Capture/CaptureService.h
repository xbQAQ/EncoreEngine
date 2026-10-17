#pragma once

#include "dx12/Dx12Device.h"
#include "dx12/ResourcePool.h"
#include <string>

namespace render {

class CaptureService {
public:
    // CaptureService borrows the device and pool; their owner must outlive this service.
    CaptureService(dx12::Dx12Device& device, dx12::ResourcePool& resources);
    ~CaptureService() = default;

    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    // Copy one texture through RDG, encode it as PNG and print capture statistics.
    void Capture(dx12::GpuResource& image, const std::wstring& path);

private:
    dx12::Dx12Device& device_; // Queue and command-list owner used for the synchronous copy.
    dx12::ResourcePool& resources_; // Pool that owns the reusable readback allocation.
    dx12::GpuResource* readback_{}; // Persistently reused CPU-readback buffer, resized when necessary.
    UINT64 readbackCapacity_{}; // Allocated byte capacity of readback_->object.
};

}
