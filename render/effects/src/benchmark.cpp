#include "directml_context.h"
#include "color_grade_compute.h"
#include "color_grade_effect.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <memory>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

// Helper to create a 2D Texture on DEFAULT heap
ComPtr<ID3D12Resource> CreateTexture2D(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState
) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource)
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create Texture2D. hr = " + std::to_string(hr));
    }
    return resource;
}

// Helper to create a Buffer (Upload/Readback)
ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device* device,
    UINT64 size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState
) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource)
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create Buffer. hr = " + std::to_string(hr));
    }
    return resource;
}

// Struct to store percentile results
struct Stats {
    double avg;
    double p95;
    double p99;
};

Stats CalculateStats(std::vector<double>& times) {
    double sum = 0.0;
    for (double t : times) {
        sum += t;
    }
    double avg = sum / times.size();

    std::sort(times.begin(), times.end());
    size_t idx95 = static_cast<size_t>(times.size() * 0.95);
    size_t idx99 = static_cast<size_t>(times.size() * 0.99);

    if (idx95 >= times.size()) idx95 = times.size() - 1;
    if (idx99 >= times.size()) idx99 = times.size() - 1;

    return { avg, times[idx95], times[idx99] };
}

// Generate the synthetic test frame gradient
void GenerateGradientFrame(float* rgba, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx = (y * width + x) * 4;
            rgba[idx + 0] = static_cast<float>(x) / width;
            rgba[idx + 1] = static_cast<float>(y) / height;
            rgba[idx + 2] = 0.5f;
            rgba[idx + 3] = 1.0f;
        }
    }
}

// Execute HLSL compute shader path on a frame and return output
void RunHLSLPath(
    DirectMLContext& context,
    ColorGradeCompute& compute,
    uint32_t width, uint32_t height,
    const float* inputFrame,
    float* outputFrame,
    const ColorGradeCompute::Params& params
) {
    ID3D12Device* device = context.GetD3D12Device();
    UINT64 sizeInBytes = width * height * 4 * sizeof(float);

    // Create GPU resources
    ComPtr<ID3D12Resource> tex2D = CreateTexture2D(
        device, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON
    );

    ComPtr<ID3D12Resource> uploadBuf = CreateBuffer(
        device, sizeInBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ
    );

    ComPtr<ID3D12Resource> readbackBuf = CreateBuffer(
        device, sizeInBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST
    );

    // Copy to upload
    void* mapped = nullptr;
    uploadBuf->Map(0, nullptr, &mapped);
    std::memcpy(mapped, inputFrame, sizeInBytes);
    uploadBuf->Unmap(0, nullptr);

    // Command objects
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    // Copy Upload -> DEFAULT Texture
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex2D.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dstCopy = {};
    dstCopy.pResource = tex2D.Get();
    dstCopy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstCopy.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcCopy = {};
    srcCopy.pResource = uploadBuf.Get();
    srcCopy.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcCopy.PlacedFootprint.Offset = 0;
    srcCopy.PlacedFootprint.Footprint.Width = width;
    srcCopy.PlacedFootprint.Footprint.Height = height;
    srcCopy.PlacedFootprint.Footprint.Depth = 1;
    srcCopy.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srcCopy.PlacedFootprint.Footprint.RowPitch = width * 4 * sizeof(float);

    cmdList->CopyTextureRegion(&dstCopy, 0, 0, 0, &srcCopy, nullptr);

    // Transition DEFAULT Texture -> UAV
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList->ResourceBarrier(1, &barrier);

    // Apply compute shader
    compute.apply(cmdList.Get(), tex2D.Get(), width, height, params);

    // Transition DEFAULT Texture -> COPY_SOURCE
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList->ResourceBarrier(1, &barrier);

    // Copy DEFAULT Texture -> Readback Buffer
    D3D12_TEXTURE_COPY_LOCATION dstRead = {};
    dstRead.pResource = readbackBuf.Get();
    dstRead.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstRead.PlacedFootprint.Offset = 0;
    dstRead.PlacedFootprint.Footprint.Width = width;
    dstRead.PlacedFootprint.Footprint.Height = height;
    dstRead.PlacedFootprint.Footprint.Depth = 1;
    dstRead.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dstRead.PlacedFootprint.Footprint.RowPitch = width * 4 * sizeof(float);

    D3D12_TEXTURE_COPY_LOCATION srcRead = {};
    srcRead.pResource = tex2D.Get();
    srcRead.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcRead.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstRead, 0, 0, 0, &srcRead, nullptr);

    // Transition back to COMMON
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList->ResourceBarrier(1, &barrier);

    cmdList->Close();
    context.ExecuteCommandList(cmdList.Get());
    context.FlushGPU();

    // Map readback
    void* readbackMapped = nullptr;
    readbackBuf->Map(0, nullptr, &readbackMapped);
    std::memcpy(outputFrame, readbackMapped, sizeInBytes);
    readbackBuf->Unmap(0, nullptr);
}

int main() {
    try {
        std::cout << "Initializing Direct3D 12 & DirectML context..." << std::endl;
        DirectMLContext context;
        ID3D12Device* device = context.GetD3D12Device();

        // ════════════════════════════════════════════════════════════
        // 1. CORRECTNESS CHECK (1080p)
        // ════════════════════════════════════════════════════════════
        std::cout << "\n[Check 1] Correctness Check (1080p)..." << std::endl;
        const uint32_t w1080 = 1920;
        const uint32_t h1080 = 1080;
        const uint32_t pixels1080 = w1080 * h1080;

        std::vector<float> input1080(pixels1080 * 4);
        GenerateGradientFrame(input1080.data(), w1080, h1080);

        ColorGradeParameters neon_params;
        neon_params.exposure = 0.5f;
        neon_params.contrast = 1.2f;
        neon_params.temperature = 0.1f;
        neon_params.tint = -0.05f;
        neon_params.saturation = 1.3f;

        ColorGradeCompute::Params compute_params;
        compute_params.exposure = 0.5f;
        compute_params.contrast = 1.2f;
        compute_params.temperature = 0.1f;
        compute_params.tint = -0.05f;
        compute_params.saturation = 1.3f;

        ColorGradeEffect effect(context, w1080, h1080);
        ColorGradeCompute compute(device, w1080, h1080);

        std::vector<float> neon_output(pixels1080 * 4, 0.0f);
        std::vector<float> hlsl_output(pixels1080 * 4, 0.0f);

        // Run Path A: HLSL Compute
        RunHLSLPath(context, compute, w1080, h1080, input1080.data(), hlsl_output.data(), compute_params);

        // Run Path B: CPU NEON
        effect.ProcessFrameCPU(input1080.data(), neon_output.data(), neon_params);

        // Calculate max_abs_diff
        float max_abs_diff = 0.0f;
        struct DiffPixel {
            uint32_t x, y;
            float r_h, g_h, b_h, a_h;
            float r_n, g_n, b_n, a_n;
            float diff;
        };
        std::vector<DiffPixel> diff_pixels;

        for (uint32_t y = 0; y < h1080; ++y) {
            for (uint32_t x = 0; x < w1080; ++x) {
                uint32_t idx = (y * w1080 + x) * 4;
                float d_r = std::abs(hlsl_output[idx + 0] - neon_output[idx + 0]);
                float d_g = std::abs(hlsl_output[idx + 1] - neon_output[idx + 1]);
                float d_b = std::abs(hlsl_output[idx + 2] - neon_output[idx + 2]);
                float pixel_max_diff = std::max({d_r, d_g, d_b});
                max_abs_diff = std::max(max_abs_diff, pixel_max_diff);

                if (pixel_max_diff >= 1e-4f && diff_pixels.size() < 5) {
                    diff_pixels.push_back({
                        x, y,
                        hlsl_output[idx + 0], hlsl_output[idx + 1], hlsl_output[idx + 2], hlsl_output[idx + 3],
                        neon_output[idx + 0], neon_output[idx + 1], neon_output[idx + 2], neon_output[idx + 3],
                        pixel_max_diff
                    });
                }
            }
        }

        bool correctness_pass = (max_abs_diff < 1e-4f);
        std::cout << "Correctness Match: " << (correctness_pass ? "PASS" : "FAIL") 
                  << " (max_abs_diff = " << max_abs_diff << ")" << std::endl;

        if (!correctness_pass) {
            std::cout << "First 5 differing pixels details:" << std::endl;
            for (const auto& dp : diff_pixels) {
                std::cout << "  At pixel (" << dp.x << ", " << dp.y << "):" << std::endl;
                std::cout << "    HLSL: [" << dp.r_h << ", " << dp.g_h << ", " << dp.b_h << ", " << dp.a_h << "]" << std::endl;
                std::cout << "    NEON: [" << dp.r_n << ", " << dp.g_n << ", " << dp.b_n << ", " << dp.a_n << "]" << std::endl;
                std::cout << "    Diff: " << dp.diff << std::endl;
            }
        }

        // ════════════════════════════════════════════════════════════
        // 2. EDGE CASE CORRECTNESS
        // ════════════════════════════════════════════════════════════
        std::cout << "\n[Check 2] Edge Case Correctness..." << std::endl;
        bool edge_pass_a = true;
        bool edge_pass_b = true;
        bool edge_pass_c = true;

        // a. exposure = 5.0
        {
            ColorGradeParameters np = {}; np.exposure = 5.0f;
            ColorGradeCompute::Params cp = {}; cp.exposure = 5.0f;
            std::vector<float> ho(pixels1080 * 4, 0.0f);
            std::vector<float> no(pixels1080 * 4, 0.0f);

            RunHLSLPath(context, compute, w1080, h1080, input1080.data(), ho.data(), cp);
            effect.ProcessFrameCPU(input1080.data(), no.data(), np);

            // Verify they agree
            float diff = 0.0f;
            for (size_t i = 0; i < ho.size(); ++i) {
                diff = std::max(diff, std::abs(ho[i] - no[i]));
            }
            // Verify if all clamp to 1.0 (excluding boundary at x=0/y=0 where input is exactly 0)
            bool hlsl_clamps = true;
            bool neon_clamps = true;
            for (uint32_t y = 1; y < h1080; ++y) {
                for (uint32_t x = 1; x < w1080; ++x) {
                    uint32_t idx = (y * w1080 + x) * 4;
                    // Exclude alpha
                    if (ho[idx + 0] < 0.999f || ho[idx + 1] < 0.999f || ho[idx + 2] < 0.999f) hlsl_clamps = false;
                    if (no[idx + 0] < 0.999f || no[idx + 1] < 0.999f || no[idx + 2] < 0.999f) neon_clamps = false;
                }
            }

            edge_pass_a = (diff < 1e-4f) && hlsl_clamps && neon_clamps;
            std::cout << "  Edge Case a (exposure=5.0): " << (edge_pass_a ? "PASS" : "FAIL") 
                      << " (diff=" << diff << ", hlsl_clamps=" << hlsl_clamps << ", neon_clamps=" << neon_clamps << ")" << std::endl;
        }

        // b. saturation = 0.0
        {
            ColorGradeParameters np = {}; np.saturation = 0.0f;
            ColorGradeCompute::Params cp = {}; cp.saturation = 0.0f;
            std::vector<float> ho(pixels1080 * 4, 0.0f);
            std::vector<float> no(pixels1080 * 4, 0.0f);

            RunHLSLPath(context, compute, w1080, h1080, input1080.data(), ho.data(), cp);
            effect.ProcessFrameCPU(input1080.data(), no.data(), np);

            float diff = 0.0f;
            for (size_t i = 0; i < ho.size(); ++i) {
                diff = std::max(diff, std::abs(ho[i] - no[i]));
            }

            bool hlsl_gray = true;
            bool neon_gray = true;
            for (uint32_t i = 0; i < pixels1080; ++i) {
                uint32_t idx = i * 4;
                if (std::abs(ho[idx + 0] - ho[idx + 1]) > 1e-4f || std::abs(ho[idx + 1] - ho[idx + 2]) > 1e-4f) hlsl_gray = false;
                if (std::abs(no[idx + 0] - no[idx + 1]) > 1e-4f || std::abs(no[idx + 1] - no[idx + 2]) > 1e-4f) neon_gray = false;
            }

            edge_pass_b = (diff < 1e-4f) && hlsl_gray && neon_gray;
            std::cout << "  Edge Case b (saturation=0.0): " << (edge_pass_b ? "PASS" : "FAIL") 
                      << " (diff=" << diff << ", hlsl_gray=" << hlsl_gray << ", neon_gray=" << neon_gray << ")" << std::endl;
        }

        // c. contrast = 0.0
        {
            ColorGradeParameters np = {}; np.contrast = 0.0f;
            ColorGradeCompute::Params cp = {}; cp.contrast = 0.0f;
            std::vector<float> ho(pixels1080 * 4, 0.0f);
            std::vector<float> no(pixels1080 * 4, 0.0f);

            RunHLSLPath(context, compute, w1080, h1080, input1080.data(), ho.data(), cp);
            effect.ProcessFrameCPU(input1080.data(), no.data(), np);

            float diff = 0.0f;
            for (size_t i = 0; i < ho.size(); ++i) {
                diff = std::max(diff, std::abs(ho[i] - no[i]));
            }

            bool hlsl_mid = true;
            bool neon_mid = true;
            for (uint32_t i = 0; i < pixels1080; ++i) {
                uint32_t idx = i * 4;
                if (std::abs(ho[idx + 0] - 0.5f) > 1e-4f || std::abs(ho[idx + 1] - 0.5f) > 1e-4f || std::abs(ho[idx + 2] - 0.5f) > 1e-4f) hlsl_mid = false;
                if (std::abs(no[idx + 0] - 0.5f) > 1e-4f || std::abs(no[idx + 1] - 0.5f) > 1e-4f || std::abs(no[idx + 2] - 0.5f) > 1e-4f) neon_mid = false;
            }

            edge_pass_c = (diff < 1e-4f) && hlsl_mid && neon_mid;
            std::cout << "  Edge Case c (contrast=0.0): " << (edge_pass_c ? "PASS" : "FAIL") 
                      << " (diff=" << diff << ", hlsl_mid=" << hlsl_mid << ", neon_mid=" << neon_mid << ")" << std::endl;
        }

        bool edge_cases_pass = edge_pass_a && edge_pass_b && edge_pass_c;

        // Initialize report variables
        double avg_1080_hlsl = 0.0;
        double avg_1080_neon = 0.0;
        double avg_4k_hlsl = 0.0;
        double avg_4k_neon = 0.0;

        // ════════════════════════════════════════════════════════════
        // 3. TIMING 1080p (only if correctness passes)
        // ════════════════════════════════════════════════════════════
        if (correctness_pass) {
            std::cout << "\n[Check 3] 1080p Timing Loop (500 iterations)..." << std::endl;
            const int iterations = 500;
            std::vector<double> hlsl_times;
            std::vector<double> neon_times;

            // Warm-up resources
            ComPtr<ID3D12Resource> tex2D = CreateTexture2D(
                device, w1080, h1080, DXGI_FORMAT_R32G32B32A32_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON
            );
            ComPtr<ID3D12Resource> uploadBuf = CreateBuffer(
                device, pixels1080 * 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ
            );
            ComPtr<ID3D12Resource> readbackBuf = CreateBuffer(
                device, pixels1080 * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST
            );

            // Command allocation cache
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> cmdList;
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));
            cmdList->Close();

            // Synch Fence
            ComPtr<ID3D12Fence> fence;
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            UINT64 fenceValue = 1;

            // HLSL Loop
            for (int i = 0; i < iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();

                allocator->Reset();
                cmdList->Reset(allocator.Get(), nullptr);

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = tex2D.Get();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmdList->ResourceBarrier(1, &barrier);

                compute.apply(cmdList.Get(), tex2D.Get(), w1080, h1080, compute_params);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                cmdList->ResourceBarrier(1, &barrier);

                cmdList->Close();
                context.ExecuteCommandList(cmdList.Get());
                
                context.GetCommandQueue()->Signal(fence.Get(), fenceValue);
                if (fence->GetCompletedValue() < fenceValue) {
                    fence->SetEventOnCompletion(fenceValue, fenceEvent);
                    WaitForSingleObject(fenceEvent, INFINITE);
                }
                fenceValue++;

                auto end = std::chrono::high_resolution_clock::now();
                double timeMs = std::chrono::duration<double, std::milli>(end - start).count();

                if (i >= 10) { // exclude first 10 warmup
                    hlsl_times.push_back(timeMs);
                }
            }

            // CPU NEON Loop
            for (int i = 0; i < iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                effect.ProcessFrameCPU(input1080.data(), neon_output.data(), neon_params);
                auto end = std::chrono::high_resolution_clock::now();

                double timeMs = std::chrono::duration<double, std::milli>(end - start).count();

                if (i >= 10) {
                    neon_times.push_back(timeMs);
                }
            }

            CloseHandle(fenceEvent);

            Stats hlsl_stats = CalculateStats(hlsl_times);
            Stats neon_stats = CalculateStats(neon_times);

            avg_1080_hlsl = hlsl_stats.avg;
            avg_1080_neon = neon_stats.avg;

            std::cout << "[HLSL Compute]  avg: " << std::fixed << std::setprecision(2) << hlsl_stats.avg << "ms  p95: " << hlsl_stats.p95 << "ms  p99: " << hlsl_stats.p99 << "ms" << std::endl;
            std::cout << "[CPU NEON    ]  avg: " << std::fixed << std::setprecision(2) << neon_stats.avg << "ms  p95: " << neon_stats.p95 << "ms  p99: " << neon_stats.p99 << "ms" << std::endl;
            std::cout << "Speedup: " << std::fixed << std::setprecision(1) << (neon_stats.avg / hlsl_stats.avg) << "x" << std::endl;
        } else {
            std::cout << "\n[Check 3] Skipped due to correctness failure." << std::endl;
        }

        // ════════════════════════════════════════════════════════════
        // 4. 4K TIMING (3840×2160)
        // ════════════════════════════════════════════════════════════
        if (correctness_pass) {
            std::cout << "\n[Check 4] 4K Timing Loop (500 iterations)..." << std::endl;
            const uint32_t w4k = 3840;
            const uint32_t h4k = 2160;
            const uint32_t pixels4k = w4k * h4k;

            std::vector<float> input4k(pixels4k * 4);
            GenerateGradientFrame(input4k.data(), w4k, h4k);
            std::vector<float> neon_output_4k(pixels4k * 4, 0.0f);

            ColorGradeEffect effect4k(context, w4k, h4k);
            ColorGradeCompute compute4k(device, w4k, h4k);

            const int iterations = 500;
            std::vector<double> hlsl_times_4k;
            std::vector<double> neon_times_4k;

            ComPtr<ID3D12Resource> tex2D = CreateTexture2D(
                device, w4k, h4k, DXGI_FORMAT_R32G32B32A32_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON
            );

            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> cmdList;
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));
            cmdList->Close();

            ComPtr<ID3D12Fence> fence;
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            UINT64 fenceValue = 1;

            // HLSL 4K Loop
            for (int i = 0; i < iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();

                allocator->Reset();
                cmdList->Reset(allocator.Get(), nullptr);

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = tex2D.Get();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmdList->ResourceBarrier(1, &barrier);

                compute4k.apply(cmdList.Get(), tex2D.Get(), w4k, h4k, compute_params);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                cmdList->ResourceBarrier(1, &barrier);

                cmdList->Close();
                context.ExecuteCommandList(cmdList.Get());

                context.GetCommandQueue()->Signal(fence.Get(), fenceValue);
                if (fence->GetCompletedValue() < fenceValue) {
                    fence->SetEventOnCompletion(fenceValue, fenceEvent);
                    WaitForSingleObject(fenceEvent, INFINITE);
                }
                fenceValue++;

                auto end = std::chrono::high_resolution_clock::now();
                double timeMs = std::chrono::duration<double, std::milli>(end - start).count();

                if (i >= 10) {
                    hlsl_times_4k.push_back(timeMs);
                }
            }

            // CPU NEON 4K Loop
            for (int i = 0; i < iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                effect4k.ProcessFrameCPU(input4k.data(), neon_output_4k.data(), neon_params);
                auto end = std::chrono::high_resolution_clock::now();

                double timeMs = std::chrono::duration<double, std::milli>(end - start).count();

                if (i >= 10) {
                    neon_times_4k.push_back(timeMs);
                }
            }

            CloseHandle(fenceEvent);

            Stats hlsl_stats_4k = CalculateStats(hlsl_times_4k);
            Stats neon_stats_4k = CalculateStats(neon_times_4k);

            avg_4k_hlsl = hlsl_stats_4k.avg;
            avg_4k_neon = neon_stats_4k.avg;

            std::cout << "[HLSL Compute]  avg: " << std::fixed << std::setprecision(2) << hlsl_stats_4k.avg << "ms  p95: " << hlsl_stats_4k.p95 << "ms  p99: " << hlsl_stats_4k.p99 << "ms" << std::endl;
            std::cout << "[CPU NEON    ]  avg: " << std::fixed << std::setprecision(2) << neon_stats_4k.avg << "ms  p95: " << neon_stats_4k.p95 << "ms  p99: " << neon_stats_4k.p99 << "ms" << std::endl;
            std::cout << "Speedup: " << std::fixed << std::setprecision(1) << (neon_stats_4k.avg / hlsl_stats_4k.avg) << "x" << std::endl;

            if (hlsl_stats_4k.avg > 2.0) {
                std::cout << "[WARNING] HLSL average latency at 4K exceeds 2.0ms limit: " << hlsl_stats_4k.avg << "ms" << std::endl;
            }
        } else {
            std::cout << "\n[Check 4] Skipped due to correctness failure." << std::endl;
        }

        // ════════════════════════════════════════════════════════════
        // 5. REPORT FORMAT
        // ════════════════════════════════════════════════════════════
        std::cout << "\n═══════════════════════════════════════" << std::endl;
        std::cout << "COLOR GRADE BENCHMARK RESULTS" << std::endl;
        std::cout << "═══════════════════════════════════════" << std::endl;
        std::cout << "Correctness:   " << (correctness_pass ? "PASS" : "FAIL") 
                  << " (max_diff=" << std::scientific << std::setprecision(2) << max_abs_diff << std::defaultfloat << ")" << std::endl;
        std::cout << "Edge cases:    " << (edge_cases_pass ? "PASS" : "FAIL") << std::endl;
        if (correctness_pass) {
            std::cout << "1080p HLSL:    " << std::fixed << std::setprecision(2) << avg_1080_hlsl << "ms avg" << std::endl;
            std::cout << "1080p NEON:    " << avg_1080_neon << "ms avg" << std::endl;
            std::cout << "1080p Speedup: " << std::fixed << std::setprecision(1) << (avg_1080_neon / avg_1080_hlsl) << "x" << std::endl;
            std::cout << "4K HLSL:       " << std::fixed << std::setprecision(2) << avg_4k_hlsl << "ms avg" << std::endl;
            std::cout << "4K NEON:       " << avg_4k_neon << "ms avg" << std::endl;
        } else {
            std::cout << "1080p HLSL:    N/A ms avg" << std::endl;
            std::cout << "1080p NEON:    N/A ms avg" << std::endl;
            std::cout << "1080p Speedup: N/A x" << std::endl;
            std::cout << "4K HLSL:       N/A ms avg" << std::endl;
            std::cout << "4K NEON:       N/A ms avg" << std::endl;
        }
        std::cout << "═══════════════════════════════════════" << std::endl;

        return correctness_pass ? 0 : 1;

    } catch (const std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what() << std::endl;
        return 1;
    }
}
