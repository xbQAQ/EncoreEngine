#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#ifndef RENDER_SHADER_DIRECTORY
#define RENDER_SHADER_DIRECTORY L"build/shaders"
#endif

namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned CaseCount = 24;
constexpr unsigned LanesPerCase = 64;

struct ValidationResult {
    std::array<float, 4> importance; // Batch mean throughput and non-null probability.
    std::array<float, 4> quadrature; // Independent sphere integral and continuous PDF mass.
    std::array<float, 4> error; // Nonfinite count, PDF error, reciprocity error and delta probability.
    std::array<float, 4> metadata; // Case identity, trials, null probability and worst reciprocity comparison scale.
};
static_assert(sizeof(ValidationResult) == 64);

struct FenceEvent {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr); // Auto-reset completion event owned by this scope.
    ~FenceEvent() { if (handle) CloseHandle(handle); }
    FenceEvent() { if (!handle) throw std::runtime_error("Cannot create GPU fence event"); }
    FenceEvent(const FenceEvent&) = delete;
    FenceEvent& operator=(const FenceEvent&) = delete;
};

struct Estimate {
    double mean = 0; // Average over independent shader batches.
    double varianceOfMean = 0; // Estimated uncertainty after reducing all batches.
};

// Preserve HRESULT context in failures so a missing shader model is distinct from a numerical regression.
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

// Hardware is preferred, while WARP keeps the numerical test usable on a headless Windows agent.
ComPtr<ID3D12Device> CreateValidationDevice() {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create DXGI factory");
    for (unsigned index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        Check(hr, "Enumerate validation adapter");
        DXGI_ADAPTER_DESC1 description{};
        Check(adapter->GetDesc1(&description), "Read adapter description");
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) continue;
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) && model.HighestShaderModel >= D3D_SHADER_MODEL_6_5) {
            std::wcout << L"BSDF validation adapter: " << description.Description << L'\n';
            return device;
        }
    }
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "Enumerate WARP adapter");
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Create WARP validation device");
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    Check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)), "Query WARP shader model");
    Require(model.HighestShaderModel >= D3D_SHADER_MODEL_6_5, "WARP does not support shader model 6.5");
    std::cout << "BSDF validation adapter: WARP\n";
    return device;
}

// Structured root UAVs need no descriptor heap; their shader-declared stride defines the record layout.
std::vector<ValidationResult> RunShader(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "Cannot read Disney validation shader: " + path.string());
    const std::vector<char> shader{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Require(!shader.empty(), "Disney validation shader is empty");
    auto device = CreateValidationDevice();
    ComPtr<ID3D12InfoQueue> info;
    device.As(&info);
    if (info) info->ClearStoredMessages();

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1;
    rootDescription.pParameters = &rootParameter;
    ComPtr<ID3DBlob> rootBytes, rootErrors;
    Check(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &rootErrors), "Serialize validation root signature");
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)), "Create validation root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = root.Get();
    pipelineDescription.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    Check(device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline)), "Create validation pipeline");

    const size_t byteCount = CaseCount * LanesPerCase * sizeof(ValidationResult);
    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = byteCount;
    bufferDescription.Height = 1;
    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels = 1;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> output;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&output)), "Allocate validation output");
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    bufferDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&readback)), "Allocate validation readback");

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "Create validation queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create validation allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&commands)), "Create validation command list");
    commands->SetComputeRootSignature(root.Get());
    commands->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    commands->Dispatch(CaseCount, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, byteCount);
    Check(commands->Close(), "Close validation command list");
    ID3D12CommandList* submission[]{commands.Get()};
    queue->ExecuteCommandLists(1, submission);
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create validation fence");
    FenceEvent completed;
    Check(queue->Signal(fence.Get(), 1), "Signal validation fence");
    Check(fence->SetEventOnCompletion(1, completed.handle), "Set validation completion event");
    Require(WaitForSingleObject(completed.handle, 60000) == WAIT_OBJECT_0, "BSDF validation GPU timed out");
    Check(device->GetDeviceRemovedReason(), "Validation device status");

    // A successful compute dispatch must also be free of debug-layer errors before its statistics are trusted.
    if (info) {
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            Check(info->GetMessage(i, nullptr, &size), "Read validation debug message size");
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            Check(info->GetMessage(i, message, &size), "Read validation debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
    }
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, byteCount};
    Check(readback->Map(0, &readRange, &mapped), "Map validation output");
    std::vector<ValidationResult> results(CaseCount * LanesPerCase);
    std::memcpy(results.data(), mapped, byteCount);
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    return results;
}

// Independent batches estimate integration uncertainty without assuming bounded microfacet tails.
template<class Accessor>
Estimate Reduce(const std::vector<ValidationResult>& data, unsigned index, Accessor value) {
    Estimate result;
    for (unsigned lane = 0; lane < LanesPerCase; ++lane) result.mean += value(data[index * LanesPerCase + lane]);
    result.mean /= LanesPerCase;
    for (unsigned lane = 0; lane < LanesPerCase; ++lane) {
        const double difference = value(data[index * LanesPerCase + lane]) - result.mean;
        result.varianceOfMean += difference * difference;
    }
    result.varianceOfMean /= LanesPerCase * (LanesPerCase - 1);
    return result;
}
bool Compatible(Estimate a, Estimate b, double absoluteTolerance) {
    return std::abs(a.mean - b.mean) <= absoluteTolerance + 7 * std::sqrt(a.varianceOfMean + b.varianceOfMean);
}

// These analytic checks are independent of Disney.hlsli and include the discrete eta-squared radiance term.
double ReferenceFresnel(double cosine, double etaI, double etaT) {
    const double sinT2 = (etaI * etaI / (etaT * etaT)) * (1 - cosine * cosine);
    if (sinT2 >= 1) return 1;
    const double ct = std::sqrt(1 - sinT2);
    const double rp = (etaT * cosine - etaI * ct) / (etaT * cosine + etaI * ct);
    const double rs = (etaI * cosine - etaT * ct) / (etaI * cosine + etaT * ct);
    return 0.5 * (rp * rp + rs * rs);
}

// Delta and very sharp GTR1 cases still check all finite/PDF invariants; sphere quadrature cannot resolve their tails reliably.
void Validate(const std::vector<ValidationResult>& data) {
    const std::array<const char*, CaseCount> names{
        "diffuse", "white dielectric", "anisotropic metal", "clearcoat", "rough glass entry", "rough glass exit",
        "delta glass IOR 4", "delta TIR", "delta metal", "sheen", "local subsurface", "thin diffuse", "thin rough glass",
        "grazing anisotropy", "rotated anisotropy", "zero base", "mixed lobes", "specular tint", "negative anisotropy",
        "interior rotated anisotropy", "sharp coat", "thin mixed", "delta plus diffuse", "fallback tangent"};
    for (unsigned index = 0; index < CaseCount; ++index) {
        double maxPdfError = 0, maxReciprocity = 0, worstReciprocityScale = 0;
        for (unsigned lane = 0; lane < LanesPerCase; ++lane) {
            const auto& record = data[index * LanesPerCase + lane];
            for (const auto* values : {&record.importance, &record.quadrature, &record.error, &record.metadata})
                for (float value : *values) Require(std::isfinite(value), "Nonfinite readback in case " + std::to_string(index));
            Require(record.metadata[0] == static_cast<float>(index) && record.metadata[1] == 256, "Validation output contract mismatch");
            Require(record.error[0] == 0, "BSDF returned nonfinite or negative values in case " + std::to_string(index));
            maxPdfError = std::max(maxPdfError, static_cast<double>(record.error[1]));
            if (record.error[2] > maxReciprocity) {
                maxReciprocity = record.error[2];
                worstReciprocityScale = record.metadata[3];
            }
        }
        Require(maxPdfError < 0.003, "Sample/evaluate PDF mismatch in case " + std::to_string(index));
        Require(maxReciprocity < 0.003, "BSDF reciprocity mismatch in case " + std::to_string(index) + ": " + std::to_string(maxReciprocity) + " at scale " + std::to_string(worstReciprocityScale));
        const auto pdf = Reduce(data, index, [](const auto& r) { return r.quadrature[3]; });
        const auto continuous = Reduce(data, index, [](const auto& r) { return r.importance[3] - r.error[3]; });
        const bool unresolvedSharpCoat = index == 20;
        if (!unresolvedSharpCoat)
            Require(Compatible(pdf, continuous, 0.025), "PDF integral disagrees with accepted continuous probability in case " + std::to_string(index));

        const bool hasDelta = index == 6 || index == 7 || index == 8 || index == 22;
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto importance = Reduce(data, index, [channel](const auto& r) { return r.importance[channel]; });
            const auto quadrature = Reduce(data, index, [channel](const auto& r) { return r.quadrature[channel]; });
            Require(importance.mean >= 0, "Negative hemispherical response");
            if (!hasDelta && !unresolvedSharpCoat)
                Require(Compatible(importance, quadrature, 0.025), "Independent energy integral disagrees with sampling in case " + std::to_string(index));
        }
        const auto energy = Reduce(data, index, [](const auto& r) { return r.importance[0]; });
        std::cout << std::setw(2) << index << ' ' << names[index] << ": energyR=" << energy.mean
            << " pdf=" << pdf.mean << " accepted=" << continuous.mean << " pdfError=" << maxPdfError
            << " reciprocity=" << maxReciprocity << '\n';
    }
    const double cosine = 1 / std::sqrt(1 + 0.45 * 0.45 + 0.2 * 0.2);
    // The highest supported IOR must reach Fresnel and radiance transport without an undocumented shader clamp.
    constexpr double glassIor = 4;
    const double fresnel = ReferenceFresnel(cosine, 1, glassIor);
    const auto glass = Reduce(data, 6, [](const auto& r) { return r.importance[0]; });
    Require(Compatible(glass, Estimate{fresnel + (1 - fresnel) / (glassIor * glassIor), 0}, 0.004), "Delta glass radiance eta-squared factor is incorrect");
    const auto tir = Reduce(data, 7, [](const auto& r) { return r.importance[0]; });
    Require(std::abs(tir.mean - 1) < 1e-5, "Ideal dielectric TIR must reflect unit throughput");
    const auto mirror = Reduce(data, 8, [](const auto& r) { return r.importance[0]; });
    const double schlick = 0.45 + 0.55 * std::pow(1 - cosine, 5);
    Require(std::abs(mirror.mean - schlick) < 1e-5, "Ideal metallic mirror differs from analytic Schlick response");
}
}

// An explicit shader argument supports independent compilation outside the CMake output tree.
int wmain(int argc, wchar_t** argv) {
    try {
        const auto shader = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(RENDER_SHADER_DIRECTORY) / L"DisneyValidation_test.cso";
        const auto results = RunShader(shader);
        std::cout << std::fixed << std::setprecision(6);
        Validate(results);
        std::cout << "Disney GPU numerical validation passed: 24 cases, 393216 scattering trials\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Disney GPU validation failed: " << error.what() << '\n';
        return 1;
    }
}
