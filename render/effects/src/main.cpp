#include "directml_context.h"
#include "color_grade_effect.h"
#include "texture_pool.h"
#include "effects_pipeline.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <random>

void GenerateMockFrame(float* rgba, uint32_t width, uint32_t height) {
    std::mt19937 rng(42); // deterministic seed
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx = (y * width + x) * 4;
            rgba[idx + 0] = dist(rng); // R
            rgba[idx + 1] = dist(rng); // G
            rgba[idx + 2] = dist(rng); // B
            rgba[idx + 3] = 1.0f;      // A
        }
    }
}

int main() {
    try {
        std::cout << "=== DirectML GPU Video Filters Engine Benchmark ===" << std::endl;

        // 1. Initialize DirectML context
        DirectMLContext context;

        // Benchmark resolutions
        std::vector<std::pair<uint32_t, uint32_t>> resolutions = {
            {1920, 1080},
            {3840, 2160}
        };

        for (const auto& res : resolutions) {
            const uint32_t width = res.first;
            const uint32_t height = res.second;
            const uint32_t pixelCount = width * height;
            std::cout << "\n=================================================" << std::endl;
            std::cout << "[Benchmark] Frame Resolution: " << width << "x" << height << " ("
                      << pixelCount << " pixels)" << std::endl;
            std::cout << "=================================================" << std::endl;

            ColorGradeEffect effect(context, width, height);

            // 3. Generate mock frame
            std::vector<float> inputFrame(pixelCount * 4);
            std::vector<float> cpuOutputFrame(pixelCount * 4, 0.0f);

            GenerateMockFrame(inputFrame.data(), width, height);
            std::cout << "[Benchmark] Mock input frame generated." << std::endl;

            // 4. Color grading parameters
            ColorGradeParameters params;
            params.exposure = 0.5f;
            params.contrast = 1.2f;
            params.saturation = 1.3f;
            params.temperature = 0.1f;
            params.tint = -0.05f;

            // 5. Benchmark CPU Path (ARM64 NEON)
            std::cout << "[Benchmark] Running CPU path..." << std::endl;

            // Warm-up
            effect.ProcessFrameCPU(inputFrame.data(), cpuOutputFrame.data(), params);

            auto cpuStart = std::chrono::high_resolution_clock::now();
            const int cpu_iterations = 10;
            for (int i = 0; i < cpu_iterations; ++i) {
                effect.ProcessFrameCPU(inputFrame.data(), cpuOutputFrame.data(), params);
            }
            auto cpuEnd = std::chrono::high_resolution_clock::now();
            double cpuTimeMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count() / cpu_iterations;

            // 6. Benchmark GPU Path (HLSL Compute)
            std::cout << "[Benchmark] Running GPU HLSL Compute path..." << std::endl;

            TexturePool pool(context.GetD3D12Device(), width, height, 8);
            int inputIdx = pool.acquire();
            int outputIdx = pool.acquire();
            if (inputIdx < 0 || outputIdx < 0) {
                throw std::runtime_error("Failed to acquire textures from the pool");
            }
            ID3D12Resource* inputTex = pool.get_resource(inputIdx);
            ID3D12Resource* outputTex = pool.get_resource(outputIdx);

            // Upload input frame to pool texture
            EffectsPipeline::upload_frame(context, inputFrame.data(), inputTex, pool.GetSizeInBytes());

            // Warm-up (triggers shader compilation and initial GPU execution)
            EffectsPipeline::apply_effects(context, effect, inputTex, outputTex, params);
            context.FlushGPU();
            std::cout << "[Benchmark] Warmup complete." << std::endl;

            double hlslTotalMs = 0.0;
            double hlslMinMs = 1e9;
            double hlslMaxMs = 0.0;

            const int hlsl_iterations = 1000;
            for (int i = 0; i < hlsl_iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                EffectsPipeline::apply_effects(context, effect, inputTex, outputTex, params);
                context.FlushGPU();
                auto end = std::chrono::high_resolution_clock::now();

                double frameMs = std::chrono::duration<double, std::milli>(end - start).count();
                hlslTotalMs += frameMs;
                if (frameMs < hlslMinMs) hlslMinMs = frameMs;
                if (frameMs > hlslMaxMs) hlslMaxMs = frameMs;
            }

            double hlslAvgMs = hlslTotalMs / hlsl_iterations;

            std::cout << "[HLSL Compute] avg: " << hlslAvgMs << " ms | min: " << hlslMinMs << " ms | max: " << hlslMaxMs << " ms" << std::endl;

            // Re-run CPU for min/max
            double cpuTotalMs = 0.0;
            double cpuMinMs = 1e9;
            double cpuMaxMs = 0.0;
            for (int i = 0; i < 10; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                effect.ProcessFrameCPU(inputFrame.data(), cpuOutputFrame.data(), params);
                auto end = std::chrono::high_resolution_clock::now();

                double frameMs = std::chrono::duration<double, std::milli>(end - start).count();
                cpuTotalMs += frameMs;
                if (frameMs < cpuMinMs) cpuMinMs = frameMs;
                if (frameMs > cpuMaxMs) cpuMaxMs = frameMs;
            }
            double cpuAvgMs = cpuTotalMs / 10;

            std::cout << "[CPU NEON    ] avg: " << cpuAvgMs << " ms | min: " << cpuMinMs << " ms | max: " << cpuMaxMs << " ms" << std::endl;
            std::cout << "Speedup: " << (cpuAvgMs / hlslAvgMs) << "x" << std::endl;

            // Download the final output frame for correctness verification
            std::vector<float> hlslOutputFrame(pixelCount * 4, 0.0f);
            EffectsPipeline::readback_frame(context, outputTex, hlslOutputFrame.data(), pool.GetSizeInBytes());

            // Release pool resources
            pool.release(inputIdx);
            pool.release(outputIdx);

            // 7. Verify correctness
            float maxDiffHlsl = 0.0f;

            for (size_t i = 0; i < cpuOutputFrame.size(); ++i) {
                float diffHlsl = std::abs(cpuOutputFrame[i] - hlslOutputFrame[i]);
                maxDiffHlsl = std::max(maxDiffHlsl, diffHlsl);
            }

            const float tolerance = 1e-4f;
            if (maxDiffHlsl < tolerance) {
                std::cout << "PASS: max_abs_diff = " << maxDiffHlsl << std::endl;
            } else {
                std::cout << "FAIL: max_abs_diff = " << maxDiffHlsl << std::endl;
                return 1;
            }
        }

    } catch (const std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what() << std::endl;
        return 1;
    }
}
