#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

class ColorGradeCompute {
public:
    struct Params {
        float exposure = 0.0f;     // [-5, 5], default 0.0
        float contrast = 1.0f;     // [0, 4],  default 1.0
        float temperature = 0.0f;  // [-1, 1], default 0.0
        float tint = 0.0f;         // [-1, 1], default 0.0
        float saturation = 1.0f;   // [0, 4],  default 1.0
        float _pad[3];             // 16-byte alignment
    };

    ColorGradeCompute(ID3D12Device* device, uint32_t max_width, uint32_t max_height);
    ~ColorGradeCompute();

    // Apply color grade to a texture IN-PLACE (no extra copy).
    // texture must be in D3D12_RESOURCE_STATE_UNORDERED_ACCESS before call.
    // Caller must signal a fence and wait before reading the result.
    void apply(ID3D12GraphicsCommandList* cmd_list,
               ID3D12Resource* texture,    // RGBA32F UAV
               uint32_t width, uint32_t height,
               const Params& params);

private:
    Microsoft::WRL::ComPtr<ID3D12Device>              device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>       root_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>       pso_;          // compiled from color_grade.hlsl
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      uav_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource>            cb_upload_;    // UPLOAD heap constant buffer
    Params*                                           cb_mapped_;    // persistently mapped pointer
};