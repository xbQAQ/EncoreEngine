#include "CaptureService.h"

#include "dx12/GraphExecutor.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>
#include <wincodec.h>

namespace render {
namespace {
struct ComApartment {
    bool uninitialize{}; // True when this call incremented COM's per-thread initialization count.

    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == S_OK || result == S_FALSE) {
            uninitialize = true;
            return;
        }
        // An existing STA is valid; its owner remains responsible for COM shutdown.
        if (result == RPC_E_CHANGED_MODE) return;
        dx12::Check(result, "Initialize COM for WIC capture");
    }
    ~ComApartment() {
        if (uninitialize) CoUninitialize();
    }
};

bool IsPooledResource(const dx12::ResourcePool& resources, const dx12::GpuResource* candidate) {
    for (const auto& resource : resources.All()) {
        if (resource.get() == candidate) return true;
    }
    return false;
}
}

CaptureService::CaptureService(dx12::Dx12Device& device, dx12::ResourcePool& resources)
    : device_(device), resources_(resources) {}

void CaptureService::Capture(dx12::GpuResource& image, const std::wstring& path) {
    // WIC receives packed eight-bit pixels; reject unsupported layouts before recording GPU work.
    if (path.empty()) throw std::invalid_argument("Capture path is empty");
    if (!image.object) throw std::invalid_argument("Cannot capture an empty GPU image");
    const D3D12_RESOURCE_DESC description = image.object->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.Width == 0 || description.Height == 0 ||
        description.DepthOrArraySize != 1 || description.MipLevels != 1 || description.SampleDesc.Count != 1)
        throw std::invalid_argument("Capture expects a single-mip 2D image");
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM && description.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        throw std::invalid_argument("Capture expects an RGBA8 or BGRA8 UNORM image");
    if (description.Width > std::numeric_limits<UINT>::max() / 4 ||
        description.Width * description.Height * 4 > std::numeric_limits<UINT>::max())
        throw std::out_of_range("Capture image exceeds the WIC buffer size limit");
    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 rowBytes{};
    UINT64 totalBytes{};
    device_.Native()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    if (!rows || !rowBytes || !totalBytes || !footprint.Footprint.RowPitch)
        throw std::runtime_error("Capture image has no readable footprint");
    if (footprint.Footprint.RowPitch < description.Width * 4)
        throw std::runtime_error("Capture footprint row pitch is smaller than RGBA8 data");

    // Retain a sufficiently large pool allocation so repeated screenshots do not grow GPU memory.
    if (!readback_ || readbackCapacity_ < totalBytes) {
        const std::string name = readback_ ? "Capture.Readback.Resized" : "Capture.Readback";
        readback_ = &resources_.Buffer(name, totalBytes, 0, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
        readbackCapacity_ = readback_->object->GetDesc().Width;
    }

    // Import an external backbuffer separately and restore its exact state after the RDG copy.
    const D3D12_RESOURCE_STATES originalImageState = image.state;
    device_.BeginCommands();
    RenderGraph graph;
    dx12::GraphExecutor executor(device_);
    executor.Import(graph, resources_);
    if (!IsPooledResource(resources_, &image)) executor.Import(graph, image);
    graph.AddPass("Capture.PNGReadback",
        {dx12::Read(image, D3D12_RESOURCE_STATE_COPY_SOURCE), dx12::Write(*readback_, D3D12_RESOURCE_STATE_COPY_DEST)},
        [this, &image, footprint] {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback_->object.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = image.object.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;
            device_.Commands()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }, true);
    graph.Export(readback_->graphId, D3D12_RESOURCE_STATE_COPY_DEST);
    graph.Export(image.graphId, originalImageState);
    executor.Execute(graph);
    device_.SubmitAndWait();

    // The texture footprint has padded rows; statistics and WIC operate on tightly packed rows.
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    void* mapped = nullptr;
    D3D12_RANGE readable{0, static_cast<SIZE_T>(totalBytes)};
    dx12::Check(readback_->object->Map(0, &readable, &mapped), "Map capture readback");
    const auto* sourceBytes = static_cast<const BYTE*>(mapped);
    double luminance = 0.0;
    UINT64 litPixels = 0;
    for (UINT y = 0; y < height; ++y) {
        const auto* row = sourceBytes + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        auto* destination = pixels.data() + static_cast<size_t>(y) * width * 4;
        std::copy_n(row, static_cast<size_t>(width) * 4, destination);
        for (UINT x = 0; x < width; ++x) {
            const auto* pixel = destination + static_cast<size_t>(x) * 4;
            const double red = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? pixel[2] : pixel[0];
            const double green = pixel[1];
            const double blue = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? pixel[0] : pixel[2];
            const double value = (0.2126 * red + 0.7152 * green + 0.0722 * blue) / 255.0;
            luminance += value;
            if (value > 0.01) ++litPixels;
        }
    }
    D3D12_RANGE written{0, 0};
    readback_->object->Unmap(0, &written);

    const auto file = std::filesystem::absolute(path);
    std::filesystem::create_directories(file.parent_path());
    ComApartment apartment;
    dx12::ComPtr<IWICImagingFactory> wic;
    dx12::Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "WIC factory");
    dx12::ComPtr<IWICStream> stream;
    dx12::Check(wic->CreateStream(&stream), "WIC stream");
    dx12::Check(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE), "PNG destination");
    dx12::ComPtr<IWICBitmapEncoder> encoder;
    dx12::Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    dx12::Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialize");
    dx12::ComPtr<IWICBitmapFrameEncode> frame;
    dx12::Check(encoder->CreateNewFrame(&frame, nullptr), "PNG frame");
    dx12::Check(frame->Initialize(nullptr), "PNG frame initialize");
    dx12::Check(frame->SetSize(width, height), "PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    dx12::Check(frame->SetPixelFormat(&format), "PNG pixel format");
    if (format != GUID_WICPixelFormat32bppBGRA)
        throw std::runtime_error("PNG encoder changed pixel format unexpectedly");
    // PNG's native BGRA encoder gets an explicit swizzle only for an RGBA source texture.
    if (description.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        for (size_t index = 0; index < pixels.size(); index += 4) std::swap(pixels[index], pixels[index + 2]);
    }
    dx12::Check(frame->WritePixels(height, width * 4,
        static_cast<UINT>(pixels.size()), pixels.data()), "PNG pixels");
    dx12::Check(frame->Commit(), "PNG frame commit");
    dx12::Check(encoder->Commit(), "PNG commit");

    const double pixelCount = static_cast<double>(width) * height;
    const double mean = luminance / pixelCount;
    std::cout << "Capture: " << file.string() << "; mean luminance=" << mean
              << "; nonblack pixels=" << litPixels << '\n';
    if (litPixels < static_cast<UINT64>(pixelCount / 20.0) || mean < 0.005)
        throw std::runtime_error("Capture validation failed: mostly blank image");
}
}
