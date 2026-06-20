#include "directml_context.h"
#include <iostream>

DirectMLContext::DirectMLContext() {
    Initialize();
}

DirectMLContext::~DirectMLContext() {
    FlushGPU();
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
    }
}

void DirectMLContext::Initialize() {
    // 1. Create DXGI Factory
    ComPtr<IDXGIFactory4> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create DXGI Factory. hr = " + std::to_string(hr));
    }

    // 2. Select Snapdragon Adreno GPU (Qualcomm vendor ID = 0x5143)
    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIAdapter1> adapter;
    
    std::cout << "[DirectML] Enumerating GPU adapters..." << std::endl;
    for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        
        std::wstring description(desc.Description);
        std::wcout << L"  Adapter [" << i << L"]: " << description 
                   << L" (Vendor ID: 0x" << std::hex << desc.VendorId << std::dec << L")" << std::endl;

        // Try to match Qualcomm Adreno explicitly
        if (desc.VendorId == 0x5143 || description.find(L"Adreno") != std::wstring::npos) {
            selectedAdapter = adapter;
            std::wcout << L"  -> Selected Qualcomm Adreno hardware profile." << std::endl;
            break;
        }
    }

    // Fallback to first non-software adapter if Qualcomm was not found
    if (!selectedAdapter) {
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selectedAdapter = adapter;
                std::wcout << L"  -> Qualcomm not found. Fallback to adapter: " << desc.Description << std::endl;
                break;
            }
        }
    }

    // Final fallback to nullptr (default adapter selection in D3D12CreateDevice)
    if (!selectedAdapter) {
        std::cout << "  -> Fallback to default D3D12 adapter." << std::endl;
    }

    // 3. Create D3D12 Device
    hr = D3D12CreateDevice(
        selectedAdapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_d3d12Device)
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create D3D12 Device. hr = " + std::to_string(hr));
    }

    // 4. Create Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    hr = m_d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create D3D12 Command Queue. hr = " + std::to_string(hr));
    }

    // 5. Create DirectML Device
    hr = DMLCreateDevice(
        m_d3d12Device.Get(),
        DML_CREATE_DEVICE_FLAG_NONE,
        IID_PPV_ARGS(&m_dmlDevice)
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create DirectML Device. hr = " + std::to_string(hr));
    }

    // 6. Create synchronization structures
    hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create D3D12 Fence. hr = " + std::to_string(hr));
    }
    m_fenceValue = 1;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        throw std::runtime_error("Failed to create D3D12 Fence Event");
    }

    std::cout << "[DirectML] Devices successfully initialized." << std::endl;
}

void DirectMLContext::ExecuteCommandList(ID3D12CommandList* commandList) {
    ID3D12CommandList* const commandLists[] = { commandList };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
}

void DirectMLContext::FlushGPU() {
    const UINT64 fenceValueToWait = m_fenceValue;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceValueToWait);
    if (FAILED(hr)) return;
    
    m_fenceValue++;

    if (m_fence->GetCompletedValue() < fenceValueToWait) {
        hr = m_fence->SetEventOnCompletion(fenceValueToWait, m_fenceEvent);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
}
