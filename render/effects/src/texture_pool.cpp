#include "texture_pool.h"
#include <stdexcept>
#include <string>
#include <iostream>

TexturePool::TexturePool(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t capacity)
    : m_width(width), m_height(height), m_capacity(capacity)
{
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
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = m_sizeInBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    std::cout << "[TexturePool] Allocating fixed pool of " << m_capacity 
              << " DEFAULT heap buffers (RGBA32_FLOAT equivalent, "
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
