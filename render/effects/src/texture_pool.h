#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class TexturePool {
public:
    TexturePool(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t capacity = 8);
    ~TexturePool();

    // Acquires a texture resource index. Returns -1 if none available.
    int acquire();

    // Releases a texture resource index back to the pool.
    void release(int index);

    // Gets the underlying D3D12 resource for a given index.
    ID3D12Resource* get_resource(int index) const;

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint64_t GetSizeInBytes() const { return m_sizeInBytes; }

private:
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_capacity;
    uint64_t m_sizeInBytes;

    std::vector<ComPtr<ID3D12Resource>> m_resources;
    std::vector<bool> m_inUse;
    mutable std::mutex m_mutex;
};
