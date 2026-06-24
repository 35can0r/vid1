#include "texture_pool.h"
#include <stdexcept>
#include <string>
#include <iostream>

TexturePool* TexturePool::s_instance = nullptr;

TexturePool::TexturePool(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t capacity)
    : m_width(width), m_height(height), m_capacity(capacity)
{
    s_instance = this;
    m_sizeInBytes = static_cast<uint64_t>(m_width) * m_height * 4 * sizeof(float); // RGBA32_FLOAT

    m_resources.resize(m_capacity);
    m_inUse.resize(m_capacity, false);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    std::cout << "[TexturePool] Allocating fixed pool of " << m_capacity 
              << " DEFAULT heap Texture2D resources (RGBA32_FLOAT, "
              << (m_sizeInBytes / (1024.0f * 1024.0f)) << " MB each)..." << std::endl;

    for (uint32_t i = 0; i < m_capacity; ++i) {
        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON, // Start in COMMON state
            nullptr,
            IID_PPV_ARGS(&m_resources[i])
        );
        if (FAILED(hr)) {
            throw std::runtime_error("TexturePool failed to create committed resource. hr = " + std::to_string(hr));
        }
    }

    // Create descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = m_capacity;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) throw std::runtime_error("Failed to create SRV descriptor heap");
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = m_capacity;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) throw std::runtime_error("Failed to create RTV descriptor heap");
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
    uavHeapDesc.NumDescriptors = m_capacity;
    uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&m_uavHeap));
    if (FAILED(hr)) throw std::runtime_error("Failed to create UAV descriptor heap");
    m_uavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create views for each texture
    for (uint32_t i = 0; i < m_capacity; ++i) {
        // SRV Descriptor
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += i * m_srvDescriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(m_resources[i].Get(), &srvDesc, srvHandle);

        // RTV Descriptor
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += i * m_rtvDescriptorSize;

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        device->CreateRenderTargetView(m_resources[i].Get(), &rtvDesc, rtvHandle);

        // UAV Descriptor
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = m_uavHeap->GetCPUDescriptorHandleForHeapStart();
        uavHandle.ptr += i * m_uavDescriptorSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(m_resources[i].Get(), nullptr, &uavDesc, uavHandle);
    }

    std::cout << "[TexturePool] Allocation complete. Total VRAM pre-allocated: " 
              << ((m_sizeInBytes * m_capacity) / (1024.0f * 1024.0f)) << " MB." << std::endl;
}

TexturePool::~TexturePool() {
    // ComPtrs will release resources automatically
}

int TexturePool::acquire() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < m_capacity; ++i) {
        if (!m_inUse[i]) {
            m_inUse[i] = true;
            return static_cast<int>(i);
        }
    }
    return -1; // Pool exhausted
}

void TexturePool::release(int index) {
    if (index < 0 || index >= static_cast<int>(m_capacity)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inUse[index] = false;
}

ID3D12Resource* TexturePool::get_resource(int index) const {
    if (index < 0 || index >= static_cast<int>(m_capacity)) return nullptr;
    return m_resources[index].Get();
}

int TexturePool::get_resource_index(ID3D12Resource* h) const {
    for (uint32_t i = 0; i < m_capacity; ++i) {
        if (m_resources[i].Get() == h) return static_cast<int>(i);
    }
    return -1;
}

D3D12_CPU_DESCRIPTOR_HANDLE TexturePool::get_srv_cpu(ID3D12Resource* h) const {
    int idx = get_resource_index(h);
    if (idx < 0) return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += idx * m_srvDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE TexturePool::get_srv_gpu(ID3D12Resource* h) const {
    int idx = get_resource_index(h);
    if (idx < 0) return {};
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += idx * m_srvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE TexturePool::get_rtv_cpu(ID3D12Resource* h) const {
    int idx = get_resource_index(h);
    if (idx < 0) return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += idx * m_rtvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE TexturePool::get_uav_cpu(ID3D12Resource* h) const {
    int idx = get_resource_index(h);
    if (idx < 0) return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_uavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += idx * m_uavDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE TexturePool::get_uav_gpu(ID3D12Resource* h) const {
    int idx = get_resource_index(h);
    if (idx < 0) return {};
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_uavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += idx * m_uavDescriptorSize;
    return handle;
}
