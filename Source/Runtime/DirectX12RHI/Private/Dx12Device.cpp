#include "dx12/Dx12Device.h"

#include <d3d12sdklayers.h>
#include <iostream>
#include <vector>

namespace render::dx12 {
namespace {
std::string AdapterDescription(const DXGI_ADAPTER_DESC1& description) {
    const std::wstring wide(description.Description);
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr) != length)
        throw std::runtime_error("Cannot encode the DXGI adapter description as UTF-8");
    return result;
}
}

Dx12Device::Dx12Device(bool validation) : validation_(validation) {
    // 创建 DXGI 之前启用调试层，使工厂诊断覆盖设备初始化。
    UINT factoryFlags = 0;
    if (validation_) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        } else {
            std::cerr << "D3D12 debug layer unavailable; device removal checks remain enabled\n";
        }
    }
    Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "DXGI factory");

    // 优先选择高性能硬件，并拒绝软件适配器作为渲染目标。
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; ; ++index) {
        const HRESULT result = factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        Check(result, "Enumerate DXGI adapter");
        DXGI_ADAPTER_DESC1 description{};
        Check(adapter->GetDesc1(&description), "DXGI adapter description");
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
            adapterName_ = AdapterDescription(description);
            break;
        }
        adapter.Reset();
    }
    if (!device_) throw std::runtime_error("No DirectX 12 hardware adapter found");

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 capabilities{};
    dxrSupported_ = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
        &capabilities, sizeof(capabilities))) && capabilities.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device_->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue_)), "Command queue");
    Check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)), "Command allocator");
    Check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
        IID_PPV_ARGS(&commands_)), "Command list");
    Check(commands_->Close(), "Initial command list close");
    Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Fence");
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) throw std::runtime_error("CreateEvent failed for D3D12 fence");
}

Dx12Device::~Dx12Device() {
    if (queue_ && fence_) {
        try { WaitIdle(); } catch (...) { /* 析构函数不能安全传播 GPU 诊断异常。 */ }
    }
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

void Dx12Device::BeginCommands() {
    Check(allocator_->Reset(), "Reset command allocator");
    Check(commands_->Reset(allocator_.Get(), nullptr), "Reset command list");
}

void Dx12Device::SubmitAndWait() {
    Check(commands_->Close(), "Close command list");
    ID3D12CommandList* lists[]{commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    Check(queue_->Signal(fence_.Get(), ++fenceValue_), "Signal fence");
    // 查询读回必须对应已执行的命令列表，不能仅依赖后续 WaitIdle 信号。
    lastSubmissionFenceValue_ = fenceValue_;
    if (fence_->GetCompletedValue() < fenceValue_) {
        Check(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_), "Set fence event");
        if (WaitForSingleObject(fenceEvent_, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU fence timed out");
    }
    Check(device_->GetDeviceRemovedReason(), "GPU device status");
    CheckDebugMessages();
}

void Dx12Device::WaitIdle() {
    if (!queue_ || !fence_) return;
    Check(queue_->Signal(fence_.Get(), ++fenceValue_), "Signal idle fence");
    if (fence_->GetCompletedValue() < fenceValue_) {
        Check(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_), "Set idle fence event");
        if (WaitForSingleObject(fenceEvent_, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU idle fence timed out");
    }
}

void Dx12Device::CheckDebugMessages() {
    if (!validation_) return;
    ComPtr<ID3D12InfoQueue> info;
    if (FAILED(device_.As(&info))) return;
    bool error = false;
    for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T size{};
        Check(info->GetMessage(index, nullptr, &size), "Get D3D12 debug message size");
        std::vector<char> memory(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(memory.data());
        Check(info->GetMessage(index, message, &size), "Get D3D12 debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            std::cerr << "D3D12: " << message->pDescription << '\n';
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) error = true;
    }
    info->ClearStoredMessages();
    if (error) throw std::runtime_error("D3D12 validation reported an error");
}
}
