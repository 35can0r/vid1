#include "directml_context.h"
#include "color_grade_effect.h"
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

        // 2. Setup effect dimensions
        const uint32_t width = 1920;
        const uint32_t height = 1080;
        const uint32_t pixelCount = width * height;
        std::cout << "[Benchmark] Frame Resolution: " << width << "x" << height << " (1080p, " 
                  << pixelCount << " pixels)" << std::endl;

        ColorGradeEffect effect(context, width, height);

        // 3. Generate mock frame
        std::vector<float> inputFrame(pixelCount * 4);
        std::vector<float> cpuOutputFrame(pixelCount * 4, 0.0f);
        std::vector<float> gpuOutputFrame(pixelCount * 4, 0.0f);

        GenerateMockFrame(inputFrame.data(), width, height);
        std::cout << "[Benchmark] Mock input frame generated." << std::endl;

        // 4. Color grading parameters
        ColorGradeParameters params;
        params.exposure = 0.5f;
        params.contrast = 1.2f;
        params.saturation = 1.3f;
        params.temperature = 0.1f;
        params.tint = -0.05f;

        std::cout << "[Benchmark] Parameters: Exposure=" << params.exposure 
                  << ", Contrast=" << params.contrast 
                  << ", Saturation=" << params.saturation 
                  << ", Temperature=" << params.temperature 
                  << ", Tint=" << params.tint << std::endl;

        // 5. Benchmark CPU Path (ARM64 NEON)
        std::cout << "[Benchmark] Running CPU path..." << std::endl;
        
        // Warm-up
        effect.ProcessFrameCPU(inputFrame.data(), cpuOutputFrame.data(), params);

        auto cpuStart = std::chrono::high_resolution_clock::now();
        const int iterations = 10;
        for (int i = 0; i < iterations; ++i) {
            effect.ProcessFrameCPU(inputFrame.data(), cpuOutputFrame.data(), params);
        }
        auto cpuEnd = std::chrono::high_resolution_clock::now();
        double cpuTimeMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count() / iterations;

        std::cout << "[Benchmark] CPU (NEON) Average Time: " << cpuTimeMs << " ms" << std::endl;

        // 6. Benchmark GPU Path (DirectML)
        std::cout << "[Benchmark] Running GPU path (DirectML)..." << std::endl;
        
        // Warm-up (triggers graph compilation and buffer allocation)
        effect.ProcessFrameGPU(inputFrame.data(), gpuOutputFrame.data(), params);
        std::cout << "[Benchmark] Warmup/Compilation complete." << std::endl;

        auto gpuStart = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            effect.ProcessFrameGPU(inputFrame.data(), gpuOutputFrame.data(), params);
        }
        auto gpuEnd = std::chrono::high_resolution_clock::now();
        double gpuTimeMs = std::chrono::duration<double, std::milli>(gpuEnd - gpuStart).count() / iterations;

        std::cout << "[Benchmark] GPU (DirectML) Average Time: " << gpuTimeMs << " ms" << std::endl;
        std::cout << "[Benchmark] GPU Speedup: " << (cpuTimeMs / gpuTimeMs) << "x" << std::endl;

        // 7. Verify correctness
        float maxDiff = 0.0f;
        float diffSum = 0.0f;
        for (size_t i = 0; i < gpuOutputFrame.size(); ++i) {
            float diff = std::abs(cpuOutputFrame[i] - gpuOutputFrame[i]);
            maxDiff = std::max(maxDiff, diff);
            diffSum += diff;
        }
        float avgDiff = diffSum / gpuOutputFrame.size();

        std::cout << "[Benchmark] Verification Results:" << std::endl;
        std::cout << "  Max absolute difference: " << maxDiff << std::endl;
        std::cout << "  Average absolute difference: " << avgDiff << std::endl;

        const float tolerance = 1e-4f;
        if (maxDiff < tolerance) {
            std::cout << "[SUCCESS] GPU and CPU outputs match within tolerance (" << tolerance << ")." << std::endl;
            return 0;
        } else {
            std::cout << "[ERROR] GPU and CPU outputs exceed tolerance!" << std::endl;
            return 1;
        }

    } catch (const std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what() << std::endl;
        return 1;
    }
}
