#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <DirectML.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

class DirectMLContext {
public:
    DirectMLContext();
    ~DirectMLContext();

    ID3D12Device* GetD3D12Device() const { return m_d3d12Device.Get(); }
    IDMLDevice* GetDMLDevice() const { return m_dmlDevice.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }

    void ExecuteCommandList(ID3D12CommandList* commandList);
    void FlushGPU();

private:
    void Initialize();

    ComPtr<ID3D12Device> m_d3d12Device;
    ComPtr<IDMLDevice> m_dmlDevice;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
};
