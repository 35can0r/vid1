#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>
#include "../include/compositor.h"

using Microsoft::WRL::ComPtr;

// Per-layer constant buffer structure matching HLSL
struct alignas(256) LayerCBData {
    float quad_center[2];
    float quad_half_size[2];
    float rotation;
    float opacity;
    float uv_min[2];
    float uv_max[2];
    float exposure;
    float contrast;
    float temperature;
    float tint;
    float saturation;
    float _pad[3];
};

class Compositor {
public:
    Compositor(ID3D12Device* device, ID3D12CommandQueue* queue,
               uint32_t canvas_w, uint32_t canvas_h);
    ~Compositor();

    TextureHandle* composite(const LayerDesc* layers, uint32_t count,
                            TextureHandle* output);

private:
    // DX12 objects created once in constructor
    ComPtr<ID3D12Device>            device_;
    ComPtr<ID3D12CommandQueue>      queue_;
    ComPtr<ID3D12RootSignature>     root_sig_;
    ComPtr<ID3D12PipelineState>     pso_;
    ComPtr<ID3D12DescriptorHeap>    rtv_heap_;    // for render target view
    ComPtr<ID3D12DescriptorHeap>    srv_heap_;    // for source texture SRV
    ComPtr<ID3D12Resource>          vb_;          // vertex buffer: 6 vertices (2 triangles)
    D3D12_VERTEX_BUFFER_VIEW        vb_view_;
    ComPtr<ID3D12Resource>          cb_upload_;   // UPLOAD heap CBV, persistently mapped
    LayerCBData*                    cb_mapped_;   // typed pointer into cb_upload_

    uint32_t canvas_w_, canvas_h_;
    ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    ComPtr<ID3D12CommandAllocator>    cmd_alloc_;
    ComPtr<ID3D12Fence>               fence_;
    HANDLE                            fence_event_;
    uint64_t                          fence_val_ = 0;

    void compile_shaders();
    void create_root_signature();
    void create_pso(); // Assumes shaders are loaded inside
    void create_vertex_buffer();   // unit quad: 6 vertices
    void wait_gpu();
};
