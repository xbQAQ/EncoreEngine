#pragma once

#include "dx12/Dx12Common.h"
#include <windows.h>

namespace render::dx12 {

class Dx12Device {
public:
    // 创建适配器、direct 队列及可复用命令录制上下文。
    explicit Dx12Device(bool validation);
    ~Dx12Device();

    // 队列同步和事件对象共用唯一寿命所有者。
    Dx12Device(const Dx12Device&) = delete;
    Dx12Device& operator=(const Dx12Device&) = delete;

    ID3D12Device5* Native() const { return device_.Get(); }
    IDXGIFactory6* Factory() const { return factory_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    ID3D12GraphicsCommandList4* Commands() const { return commands_.Get(); }
    bool DxrSupported() const { return dxrSupported_; }
    const std::string& AdapterName() const { return adapterName_; }
    // 查询读回需要区分已完成的命令列表提交与仅队列空闲 fence。
    UINT64 LastSubmissionFenceValue() const { return lastSubmissionFenceValue_; }
    UINT64 CompletedFenceValue() const { return fence_->GetCompletedValue(); }
    UINT64 SubmittedFenceValue() const { return fenceValue_; }

    // 上一提交完成后重置分配器和命令列表。
    void BeginCommands();
    // 关闭并提交当前命令列表，等待完成后返回。
    void SubmitAndWait();
    // 向队列插入 fence 并等待，不改变命令列表录制状态。
    void WaitIdle();

private:
    void CheckDebugMessages();

    ComPtr<IDXGIFactory6> factory_; // 用于适配器枚举和交换链创建的 DXGI 工厂。
    ComPtr<ID3D12Device5> device_; // 提供可选 DXR 1.1 能力的 D3D12 设备。
    ComPtr<ID3D12CommandQueue> queue_; // 图形、计算及拷贝工作共用的 direct 队列。
    ComPtr<ID3D12CommandAllocator> allocator_; // 仅在对应 fence 完成后重置的命令分配器。
    ComPtr<ID3D12GraphicsCommandList4> commands_; // 当前同步帧使用的可复用命令列表。
    ComPtr<ID3D12Fence> fence_; // 保护分配器和上传内存复用的队列时间线。
    HANDLE fenceEvent_{}; // 提交 fence 完成时通知 CPU 的自动重置事件。
    UINT64 fenceValue_{}; // 此设备最近一次发出的 fence 值。
    UINT64 lastSubmissionFenceValue_{}; // 保护最近一次实际命令列表提交的 fence 值。
    bool validation_{}; // 启用调试层，并把记录的错误消息视为失败。
    bool dxrSupported_{}; // 所选适配器支持 DXR 1.1 或更高等级时为真。
    std::string adapterName_; // 供编辑器诊断显示的 UTF-8 适配器说明。
};

}
