#pragma once
#include "directml_context.h"
#include "color_grade_effect.h"
#include "texture_pool.h"

namespace EffectsPipeline {
    // Uploads a CPU frame (float RGBA) to a GPU default-heap texture/buffer from the pool.
    void upload_frame(
        DirectMLContext& context,
        const float* cpuInputRGBA,
        ID3D12Resource* gpuTexture,
        uint64_t sizeInBytes
    );

    // Applies the color grading effects on the GPU asynchronously (no CPU stalls/fences).
    void apply_effects(
        DirectMLContext& context,
        ColorGradeEffect& effect,
        ID3D12Resource* inputTexture,
        ID3D12Resource* outputTexture,
        const ColorGradeParameters& params
    );

    // Downloads the GPU texture/buffer to CPU RAM (synchronous, for exports).
    void readback_frame(
        DirectMLContext& context,
        ID3D12Resource* gpuTexture,
        float* cpuOutputRGBA,
        uint64_t sizeInBytes
    );
}
