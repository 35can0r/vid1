#pragma once

#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>

class PreviewPresenter {
public:
    // Called once from C# via P/Invoke, passing the SwapChainPanel's
    // ISwapChainPanelNative interface (as void*)
    PreviewPresenter(void* swap_chain_panel_native,
                     ID3D12Device* device,
                     ID3D12CommandQueue* queue,
                     uint32_t width, uint32_t height);

    // Blit a compositor output texture to the swapchain and present.
    // Call after fence is signalled.
    void present(ID3D12Resource* compositor_output);

    void resize(uint32_t width, uint32_t height);

    ~PreviewPresenter();

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain1>     swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11Device>        d3d11_device_;
    Microsoft::WRL::ComPtr<ID3D11On12Device>    d3d11on12_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_ctx_;
    uint32_t                                    width_;
    uint32_t                                    height_;
};
