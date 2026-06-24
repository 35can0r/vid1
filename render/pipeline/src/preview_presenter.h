#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <stdint.h>

class PreviewPresenter {
public:
    // panelNative: ISwapChainPanelNative* (COM pointer from C#)
    PreviewPresenter(IUnknown* panelNative,
                     ID3D12Device* device,
                     ID3D12CommandQueue* queue,
                     uint32_t width, uint32_t height);
    ~PreviewPresenter();

    // Reads compositor RGBA32F texture, tonemaps to RGBA8, presents.
    // compositor_output must be in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
    void present(ID3D12Resource* compositor_output);

    void resize(uint32_t new_width, uint32_t new_height);

private:
    ID3D12Device*            device_;
    ID3D12CommandQueue*      queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>         swap_chain_;       // native D3D12 swapchain
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    rtv_heap_;         // for swapchain backbuffers
    Microsoft::WRL::ComPtr<ID3D12Resource>          back_buffers_[2];  // double-buffered
    UINT                     frame_index_ = 0;   // current backbuffer index

    // Tonemapper PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature>     tonemap_root_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>     tonemap_pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    tonemap_srv_heap_;  // SRV (input)
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>  cmd_alloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence>             fence_;
    uint64_t                 fence_val_ = 0;
    HANDLE                   fence_event_ = nullptr;
    uint32_t                 width_;
    uint32_t                 height_;

    void create_swapchain(IUnknown* panel_native, uint32_t w, uint32_t h);
    void create_tonemap_pso();
    void wait_gpu();
};
