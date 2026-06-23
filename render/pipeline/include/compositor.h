#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12CommandQueue ID3D12CommandQueue;
typedef struct ID3D12Resource TextureHandle;

// All spatial values normalized 0.0-1.0 relative to canvas
typedef struct Transform {
    float center_x;   // 0.5 = canvas center
    float center_y;
    float width;      // 1.0 = full canvas width
    float height;
    float rotation;   // radians, clockwise
} Transform;

typedef struct Crop {
    float left;       // fraction to cut from left edge
    float top;
    float right;      // fraction to cut from right edge
    float bottom;
} Crop;

typedef struct ColorGradeParams {
    float exposure;    // [-5, 5], default 0.0
    float contrast;    // [0, 4],  default 1.0
    float temperature; // [-1, 1], default 0.0
    float tint;        // [-1, 1], default 0.0
    float saturation;  // [0, 4],  default 1.0
    float _pad[3];     // 16-byte CBV alignment
} ColorGradeParams;

typedef struct LayerDesc {
    TextureHandle* texture;   // decoded frame from TexturePool
    Transform      transform;
    Crop           crop;
    ColorGradeParams grade;
    float          opacity;   // 0.0-1.0
    float          _pad[3];
} LayerDesc;

// C ABI - these signatures must not change
typedef void* CompositorHandle;

typedef struct ID3D12Fence ID3D12Fence;

typedef struct CompositeResult {
    TextureHandle* output;       // the output texture (not yet ready)
    uint64_t       fence_value;  // wait on this before reading output
    ID3D12Fence*   fence;        // the fence to wait on
} CompositeResult;

CompositorHandle compositor_create(
    ID3D12Device*       device,
    ID3D12CommandQueue* queue,
    uint32_t            canvas_width,
    uint32_t            canvas_height
);

// composite() layers bottom-to-top (layers[0] = bottom, layers[N-1] = top)
// output_texture: a TexturePool slot in D3D12_RESOURCE_STATE_RENDER_TARGET
// On return, output_texture is in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
TextureHandle* compositor_composite(
    CompositorHandle    handle,
    const LayerDesc*    layers,
    uint32_t            layer_count,
    TextureHandle*      output_texture   // pre-acquired from TexturePool
);

CompositeResult compositor_composite_async(
    CompositorHandle    handle,
    const LayerDesc*    layers,
    uint32_t            layer_count,
    TextureHandle*      output_texture
);

uint64_t compositor_current_fence_val(CompositorHandle handle);

void compositor_destroy(CompositorHandle handle);

#ifdef __cplusplus
}
#endif

