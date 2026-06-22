#pragma once
#include "directml_context.h"
#include <vector>

struct ColorGradeParameters {
    float exposure = 0.0f;     // EV units (scale = 2^exposure)
    float contrast = 1.0f;     // 0.0 to 2.0+
    float saturation = 1.0f;   // 0.0 (B&W) to 2.0+
    float temperature = 0.0f;  // shifts red/blue (-0.5 to 0.5)
    float tint = 0.0f;         // shifts green/magenta (-0.5 to 0.5)

    bool Equals(const ColorGradeParameters& other) const {
        return exposure == other.exposure &&
               contrast == other.contrast &&
               saturation == other.saturation &&
               temperature == other.temperature &&
               tint == other.tint;
    }
};

class ColorGradeEffect {
public:
    ColorGradeEffect(DirectMLContext& context, uint32_t width, uint32_t height);
    ~ColorGradeEffect();

    // GPU Processing path using DirectML
    void ProcessFrameGPU(
        const float* inputRGBA,
        float* outputRGBA,
        const ColorGradeParameters& params
    );

    // Optimized GPU Processing path using DirectML with persistent VRAM textures
    void ProcessFrameGPUTexture(
        ID3D12Resource* inputBuffer,
        ID3D12Resource* outputBuffer,
        const ColorGradeParameters& params
    );

    // CPU Fallback path using ARM64 NEON intrinsics
    void ProcessFrameCPU(
        const float* inputRGBA,
        float* outputRGBA,
        const ColorGradeParameters& params
    );

private:
    void CompileOperatorGraph(const ColorGradeParameters& params);
    void AllocateGPUResources();
    void UpdateConstantTensors(const ColorGradeParameters& params);

    DirectMLContext& m_context;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pixelCount;
    uint32_t m_tensorSizeInBytes;

    // GPU execution state
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<IDMLCompiledOperator> m_compiledOperator;
    ComPtr<IDMLBindingTable> m_bindingTable;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;

    // GPU resources
    ComPtr<ID3D12Resource> m_inputUploadBuffer;
    ComPtr<ID3D12Resource> m_inputGPUBuffer;
    ComPtr<ID3D12Resource> m_outputGPUBuffer;
    ComPtr<ID3D12Resource> m_outputReadbackBuffer;
    ComPtr<ID3D12Resource> m_persistentBuffer;
    ComPtr<ID3D12Resource> m_temporaryBuffer;

    // DirectML Graph Inputs for Scale and Bias constant tensors
    ComPtr<ID3D12Resource> m_scaleGPUBuffer;
    ComPtr<ID3D12Resource> m_scaleUploadBuffer;
    ComPtr<ID3D12Resource> m_biasGPUBuffer;
    ComPtr<ID3D12Resource> m_biasUploadBuffer;

    ColorGradeParameters m_activeParams;
    bool m_resourcesAllocated = false;
    bool m_graphCompiled = false;
};
