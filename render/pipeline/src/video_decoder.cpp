#include "video_decoder.h"
#include "../../effects/src/texture_pool.h"
#include <iostream>
#include <vector>
#include <list>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

static void LogDebug(const std::string& msg) {
    std::ofstream log("palmier_engine.log", std::ios_base::app);
    if (log.is_open()) {
        log << msg << std::endl;
    }
}

// D3D11 and D3D12 headers
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

// FFmpeg headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

using Microsoft::WRL::ComPtr;

struct CachedFrame {
    int64_t frame_index;
    AVFrame* frame;
};

struct DecoderHandle {
    std::string path;
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    const AVCodec* codec = nullptr;
    int video_stream_idx = -1;

    // Media metadata
    int32_t width = 0;
    int32_t height = 0;
    int32_t fps_num = 0;
    int32_t fps_den = 0;
    int64_t total_frames = 0;

    // D3D12 / D3D11On12 interop variables
    ID3D12Device* d3d12_device = nullptr;
    ComPtr<ID3D12CommandQueue> d3d12_queue;
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> d3d11_context;
    ComPtr<ID3D11On12Device> d3d11on12_device;

    // D3D12 command copy helpers for software upload fallback
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    ComPtr<ID3D12GraphicsCommandList> copy_cmd_list;
    ComPtr<ID3D12Resource> upload_buffer;
    uint64_t upload_buffer_size = 0;

    // D3D12 fence for synchronization
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    // FFmpeg HW context
    AVBufferRef* hw_device_ctx = nullptr;
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    bool hw_active = false;
    bool hw_init_attempted = false;

    // Compute Shader state
    ComPtr<ID3D11ComputeShader> compute_shader;
    ComPtr<ID3D11Buffer> constant_buffer;

    // Software fallback scaler
    SwsContext* sws_ctx = nullptr;

    // Packet for reading frames
    AVPacket* pkt = nullptr;

    // Seeking state
    int64_t pending_seek_frame = -1;
    int64_t current_frame = -1;
    bool draining = false;
    bool reached_eof = false;
    int64_t eof_frame = 999999999;

    // Ring buffer (LRU Cache)
    std::list<CachedFrame> cache_list;
    static const size_t MAX_CACHE_SIZE = 16;

    // Audio decoding cache
    AVCodecContext* audio_ctx = nullptr;
    const AVCodec* audio_codec = nullptr;
    int audio_stream_idx = -1;
    int64_t current_audio_frame = -1;
    int64_t current_audio_sample = -1;
    AVPacket* audio_pkt = nullptr;
    AVFrame* audio_frame = nullptr;
};

// Helper to convert 8-bit RGBA integer buffer to 32-bit float RGBA buffer on CPU
static void convert_rgba8_to_rgba32f(const uint8_t* rgba8, float* rgba32f, uint32_t width, uint32_t height) {
    uint32_t count = width * height * 4;
    for (uint32_t i = 0; i < count; ++i) {
        rgba32f[i] = rgba8[i] / 255.0f;
    }
}

// Helper to initialize D3D11On12 device wrapping our D3D12 device
static bool init_d3d11on12(DecoderHandle* h, ID3D12Device* d3d12_device) {
    h->d3d12_device = d3d12_device;

    // 1. Create a dedicated D3D12 command queue for decoding/interop processing
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = d3d12_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&h->d3d12_queue));
    if (FAILED(hr)) {
        std::cerr << "[Decoder] Failed to create D3D12 command queue: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    // 2. Call D3D11On12CreateDevice to map our D3D12 device to D3D11 commands
    IUnknown* queues[] = { h->d3d12_queue.Get() };
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosenLevel;

    hr = D3D11On12CreateDevice(
        d3d12_device,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        2,
        queues,
        1,
        0,
        &h->d3d11_device,
        &h->d3d11_context,
        &chosenLevel
    );
    if (FAILED(hr)) {
        std::cerr << "[Decoder] D3D11On12CreateDevice failed: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = h->d3d11_device.As(&h->d3d11on12_device);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] Failed to query ID3D11On12Device: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    return true;
}

// Helper to compile the GPU NV12 to RGBA32_FLOAT conversion compute shader
static bool init_compute_shader(DecoderHandle* h, bool use_array) {
    std::string shader_src;
    if (use_array) {
        shader_src = 
            "Texture2DArray<float> InputY : register(t0);\n"
            "Texture2DArray<float2> InputUV : register(t1);\n"
            "RWTexture2D<float4> OutputBuffer : register(u0);\n"
            "\n"
            "cbuffer Params : register(b0)\n"
            "{\n"
            "    uint Width;\n"
            "    uint Height;\n"
            "    uint ArrayIndex;\n"
            "    uint Padding;\n"
            "};\n"
            "\n"
            "[numthreads(16, 16, 1)]\n"
            "void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)\n"
            "{\n"
            "    if (dispatchThreadID.x >= Width || dispatchThreadID.y >= Height)\n"
            "        return;\n"
            "\n"
            "    int4 texCoord = int4(dispatchThreadID.x, dispatchThreadID.y, ArrayIndex, 0);\n"
            "    float yVal = InputY.Load(texCoord).r;\n"
            "    int4 uvCoord = int4(dispatchThreadID.x / 2, dispatchThreadID.y / 2, ArrayIndex, 0);\n"
            "    float2 uvVal = InputUV.Load(uvCoord).rg;\n"
            "\n"
            "    // Bt.709 limited range digital YUV to RGB conversion\n"
            "    float Y = (yVal - (16.0f / 255.0f)) * (255.0f / 219.0f);\n"
            "    float U = uvVal.x - 0.5f;\n"
            "    float V = uvVal.y - 0.5f;\n"
            "\n"
            "    float r = Y + 1.5748f * V;\n"
            "    float g = Y - 0.1873f * U - 0.4681f * V;\n"
            "    float b = Y + 1.8556f * U;\n"
            "\n"
            "    r = saturate(r);\n"
            "    g = saturate(g);\n"
            "    b = saturate(b);\n"
            "    float a = 1.0f;\n"
            "\n"
            "    OutputBuffer[dispatchThreadID.xy] = float4(r, g, b, a);\n"
            "}\n";
    } else {
        shader_src = 
            "Texture2D<float> InputY : register(t0);\n"
            "Texture2D<float2> InputUV : register(t1);\n"
            "RWTexture2D<float4> OutputBuffer : register(u0);\n"
            "\n"
            "cbuffer Params : register(b0)\n"
            "{\n"
            "    uint Width;\n"
            "    uint Height;\n"
            "    uint ArrayIndex;\n"
            "    uint Padding;\n"
            "};\n"
            "\n"
            "[numthreads(16, 16, 1)]\n"
            "void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)\n"
            "{\n"
            "    if (dispatchThreadID.x >= Width || dispatchThreadID.y >= Height)\n"
            "        return;\n"
            "\n"
            "    int3 texCoord = int3(dispatchThreadID.x, dispatchThreadID.y, 0);\n"
            "    float yVal = InputY.Load(texCoord).r;\n"
            "    int3 uvCoord = int3(dispatchThreadID.x / 2, dispatchThreadID.y / 2, 0);\n"
            "    float2 uvVal = InputUV.Load(uvCoord).rg;\n"
            "\n"
            "    // Bt.709 limited range digital YUV to RGB conversion\n"
            "    float Y = (yVal - (16.0f / 255.0f)) * (255.0f / 219.0f);\n"
            "    float U = uvVal.x - 0.5f;\n"
            "    float V = uvVal.y - 0.5f;\n"
            "\n"
            "    float r = Y + 1.5748f * V;\n"
            "    float g = Y - 0.1873f * U - 0.4681f * V;\n"
            "    float b = Y + 1.8556f * U;\n"
            "\n"
            "    r = saturate(r);\n"
            "    g = saturate(g);\n"
            "    b = saturate(b);\n"
            "    float a = 1.0f;\n"
            "\n"
            "    OutputBuffer[dispatchThreadID.xy] = float4(r, g, b, a);\n"
            "}\n";
    }

    ComPtr<ID3DBlob> shader_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(
        shader_src.c_str(),
        shader_src.length(),
        "NV12ToRGBA_CS",
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_0",
        0, 0,
        &shader_blob,
        &error_blob
    );
    if (FAILED(hr)) {
        if (error_blob) {
            std::cerr << "[Decoder] CS compile error: " 
                      << (const char*)error_blob->GetBufferPointer() << "\n";
        }
        return false;
    }

    hr = h->d3d11_device->CreateComputeShader(
        shader_blob->GetBufferPointer(),
        shader_blob->GetBufferSize(),
        nullptr,
        &h->compute_shader
    );
    if (FAILED(hr)) {
        std::cerr << "[Decoder] CreateComputeShader failed: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    // Create dynamic constant buffer if not already done
    if (!h->constant_buffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = h->d3d11_device->CreateBuffer(&cbDesc, nullptr, &h->constant_buffer);
        if (FAILED(hr)) {
            std::cerr << "[Decoder] Constant buffer creation failed: 0x" << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    return true;
}

// Lazy initialization of hardware acceleration
static bool ensure_initialized(DecoderHandle* h, ID3D12Device* d3d12_device) {
    if (h->hw_init_attempted) {
        return true;
    }
    h->hw_init_attempted = true;

    // 1. Try setting up D3D11On12 mapping
    if (init_d3d11on12(h, d3d12_device)) {
        h->hw_active = false;
        
        // Search through decoder configuration profiles to find standard D3D11VA
        for (int i = 0;; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(h->codec, i);
            if (!config) break;

            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                h->hw_pix_fmt = config->pix_fmt;
                
                // Allocate and configure the HW device context utilizing our custom D3D11On12 device
                AVBufferRef* device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
                if (device_ref) {
                    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)device_ref->data;
                    AVD3D11VADeviceContext* d3d11_device_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;
                    
                    d3d11_device_ctx->device = h->d3d11_device.Get();
                    d3d11_device_ctx->device->AddRef(); // FFmpeg will release it
                    
                    int err = av_hwdevice_ctx_init(device_ref);
                    if (err >= 0) {
                        h->hw_device_ctx = device_ref;
                        h->codec_ctx->hw_device_ctx = av_buffer_ref(h->hw_device_ctx);
                        
                        h->codec_ctx->opaque = (void*)h;
                        h->codec_ctx->get_format = [](AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) -> AVPixelFormat {
                            DecoderHandle* h = (DecoderHandle*)ctx->opaque;
                            const enum AVPixelFormat *p;
                            for (p = pix_fmts; *p != -1; p++) {
                                if (*p == h->hw_pix_fmt) {
                                    // Allocate and configure custom hw_frames_ctx to include D3D11_BIND_SHADER_RESOURCE
                                    AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(h->hw_device_ctx);
                                    if (hw_frames_ref) {
                                        AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
                                        frames_ctx->format = h->hw_pix_fmt;
                                        frames_ctx->sw_format = AV_PIX_FMT_NV12;
                                        frames_ctx->width = ctx->width;
                                        frames_ctx->height = ctx->height;
                                        frames_ctx->initial_pool_size = 48;

                                        AVD3D11VAFramesContext* d3d11_frames = (AVD3D11VAFramesContext*)frames_ctx->hwctx;
                                        d3d11_frames->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

                                        int err = av_hwframe_ctx_init(hw_frames_ref);
                                        if (err >= 0) {
                                            ctx->hw_frames_ctx = hw_frames_ref; // Ownership of reference transferred
                                            std::cout << "[Decoder] Custom frames context with SHADER_RESOURCE flag created.\n";
                                        } else {
                                            std::cerr << "[Decoder] av_hwframe_ctx_init failed: " << err << "\n";
                                            av_buffer_unref(&hw_frames_ref);
                                        }
                                    }
                                    return *p;
                                }
                            }
                            return AV_PIX_FMT_NONE;
                        };
                        h->hw_active = true;
                        std::cout << "[Decoder] Native D3D11VA hardware context linked successfully.\n";
                        break;
                    } else {
                        av_buffer_unref(&device_ref);
                    }
                }
            }
        }

        if (h->hw_active) {
            // Initialize sync structures
            HRESULT hr = d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h->fence));
            if (SUCCEEDED(hr)) {
                h->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            } else {
                std::cerr << "[Decoder] Failed to create sync fence. Disabling hardware acceleration.\n";
                h->hw_active = false;
            }
        }
    }

    if (!h->hw_active) {
        std::cout << "[Decoder] Hardware initialization failed. Falling back to multi-threaded CPU software decoder.\n";
        // Create fallback D3D12 fence for CPU upload synchronization
        HRESULT hr = d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h->fence));
        if (SUCCEEDED(hr)) {
            h->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
    }

    if (h->hw_active) {
        h->codec_ctx->extra_hw_frames = 32;
    }

    // Finally open the codec
    int err = avcodec_open2(h->codec_ctx, h->codec, nullptr);
    if (err < 0) {
        std::cerr << "[Decoder] avcodec_open2 failed: " << err << "\n";
        return false;
    }

    return true;
}

static bool upload_to_d3d12_texture(DecoderHandle* h, const float* float_data, ID3D12Resource* out_texture, uint64_t size_in_bytes) {
    ID3D12Device* device = h->d3d12_device;

    if (!h->upload_buffer || h->upload_buffer_size < size_in_bytes) {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = size_in_bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&h->upload_buffer)
        );
        if (FAILED(hr)) return false;
        h->upload_buffer_size = size_in_bytes;
    }

    void* mapped = nullptr;
    HRESULT hr = h->upload_buffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) return false;
    memcpy(mapped, float_data, size_in_bytes);
    h->upload_buffer->Unmap(0, nullptr);

    if (!h->copy_allocator) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&h->copy_allocator));
        if (FAILED(hr)) return false;

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h->copy_allocator.Get(), nullptr, IID_PPV_ARGS(&h->copy_cmd_list));
        if (FAILED(hr)) return false;
        h->copy_cmd_list->Close();
    }

    h->copy_allocator->Reset();
    h->copy_cmd_list->Reset(h->copy_allocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = out_texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    h->copy_cmd_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = out_texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = h->upload_buffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    src.PlacedFootprint.Footprint.Width = h->width;
    src.PlacedFootprint.Footprint.Height = h->height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = h->width * 4 * sizeof(float);

    h->copy_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    h->copy_cmd_list->ResourceBarrier(1, &barrier);

    h->copy_cmd_list->Close();

    ID3D12CommandList* lists[] = { h->copy_cmd_list.Get() };
    h->d3d12_queue->ExecuteCommandLists(1, lists);

    UINT64 fence_val = ++h->fence_value;
    h->d3d12_queue->Signal(h->fence.Get(), fence_val);
    if (h->fence->GetCompletedValue() < fence_val) {
        h->fence->SetEventOnCompletion(fence_val, h->fence_event);
        WaitForSingleObject(h->fence_event, INFINITE);
    }

    return true;
}

// Convert frame data on GPU (NV12) or CPU fallback
static ErrorCode output_frame_to_texture(DecoderHandle* h, AVFrame* frame, ID3D12Resource* out_texture) {
    if (h->hw_active && frame->format == h->hw_pix_fmt) {
        // --- GPU zero-copy path ---
        ID3D11Texture2D* hw_tex = (ID3D11Texture2D*)frame->data[0];
        intptr_t array_index = (intptr_t)frame->data[1];

        if (!hw_tex) {
            std::cerr << "[Decoder] HW texture is null.\n";
            return DECODER_ERROR_DECODE_FAILED;
        }

        // Query the texture description to check array status
        D3D11_TEXTURE2D_DESC texDesc = {};
        hw_tex->GetDesc(&texDesc);
        bool use_array = (texDesc.ArraySize > 1);

        std::cout << "[Decoder Debug] Texture Desc: Format=" << texDesc.Format 
                  << ", Width=" << texDesc.Width 
                  << ", Height=" << texDesc.Height 
                  << ", MipLevels=" << texDesc.MipLevels 
                  << ", ArraySize=" << texDesc.ArraySize 
                  << ", BindFlags=" << texDesc.BindFlags 
                  << ", array_index=" << array_index << std::endl;

        // Compile compute shader lazily if not yet compiled
        if (!h->compute_shader) {
            if (!init_compute_shader(h, use_array)) {
                std::cerr << "[Decoder] Lazy shader compilation failed. Switching to software fallback.\n";
                h->hw_active = false;
                return output_frame_to_texture(h, frame, out_texture);
            }
        }

        // Wrap D3D12 target texture for D3D11 Compute Shader UAV access
        ComPtr<ID3D11Texture2D> d3d11_tex;
        D3D11_RESOURCE_FLAGS rFlags = {};
        rFlags.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        HRESULT hr = h->d3d11on12_device->CreateWrappedResource(
            out_texture,
            &rFlags,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            IID_PPV_ARGS(&d3d11_tex)
        );
        if (FAILED(hr)) {
            std::cerr << "[Decoder] D3D11On12 CreateWrappedResource failed: 0x" << std::hex << hr << std::dec << "\n";
            return DECODER_ERROR_DECODE_FAILED;
        }

        // Bind NV12 planes (Y / UV) to D3D11 Shader Resource Views (SRVs)
        ComPtr<ID3D11ShaderResourceView> srvY;
        ComPtr<ID3D11ShaderResourceView> srvUV;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvYDesc = {};
        srvYDesc.Format = DXGI_FORMAT_R8_UNORM;
        
        if (use_array) {
            srvYDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvYDesc.Texture2DArray.FirstArraySlice = (UINT)array_index;
            srvYDesc.Texture2DArray.ArraySize = 1;
            srvYDesc.Texture2DArray.MipLevels = 1;
            srvYDesc.Texture2DArray.MostDetailedMip = 0;
        } else {
            srvYDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvYDesc.Texture2D.MipLevels = 1;
            srvYDesc.Texture2D.MostDetailedMip = 0;
        }

        hr = h->d3d11_device->CreateShaderResourceView(hw_tex, &srvYDesc, &srvY);
        if (FAILED(hr)) {
            std::cerr << "[Decoder] Create SRV Y failed: 0x" << std::hex << hr << std::dec << "\n";
            return DECODER_ERROR_DECODE_FAILED;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvUVDesc = srvYDesc;
        srvUVDesc.Format = DXGI_FORMAT_R8G8_UNORM;

        hr = h->d3d11_device->CreateShaderResourceView(hw_tex, &srvUVDesc, &srvUV);
        if (FAILED(hr)) {
            std::cerr << "[Decoder] Create SRV UV failed: 0x" << std::hex << hr << std::dec << "\n";
            return DECODER_ERROR_DECODE_FAILED;
        }

        // Create UAV for writing float data
        ComPtr<ID3D11UnorderedAccessView> uav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        hr = h->d3d11_device->CreateUnorderedAccessView(d3d11_tex.Get(), &uavDesc, &uav);
        if (FAILED(hr)) {
            std::cerr << "[Decoder] Create UAV failed: 0x" << std::hex << hr << std::dec << "\n";
            return DECODER_ERROR_DECODE_FAILED;
        }

        // Update parameters constant buffer
        D3D11_MAPPED_SUBRESOURCE mappedCB;
        hr = h->d3d11_context->Map(h->constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB);
        if (SUCCEEDED(hr)) {
            uint32_t cbData[4] = { (uint32_t)h->width, (uint32_t)h->height, (uint32_t)array_index, 0 };
            memcpy(mappedCB.pData, cbData, sizeof(cbData));
            h->d3d11_context->Unmap(h->constant_buffer.Get(), 0);
        }

        // Dispatch conversion shader
        ID3D11Resource* resourceToAcquire = d3d11_tex.Get();
        h->d3d11on12_device->AcquireWrappedResources(&resourceToAcquire, 1);

        h->d3d11_context->CSSetShader(h->compute_shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { srvY.Get(), srvUV.Get() };
        h->d3d11_context->CSSetShaderResources(0, 2, srvs);
        h->d3d11_context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        h->d3d11_context->CSSetConstantBuffers(0, 1, h->constant_buffer.GetAddressOf());

        h->d3d11_context->Dispatch((h->width + 15) / 16, (h->height + 15) / 16, 1);

        // Unbind resources
        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        h->d3d11_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr };
        h->d3d11_context->CSSetShaderResources(0, 2, nullSRVs);

        h->d3d11on12_device->ReleaseWrappedResources(&resourceToAcquire, 1);
        h->d3d11_context->Flush();

        // Synchronize on D3D12 side
        UINT64 fence_val = ++h->fence_value;
        h->d3d12_queue->Signal(h->fence.Get(), fence_val);
        if (h->fence->GetCompletedValue() < fence_val) {
            h->fence->SetEventOnCompletion(fence_val, h->fence_event);
            WaitForSingleObject(h->fence_event, INFINITE);
        }

        return DECODER_SUCCESS;
    } else {
        // --- CPU software scaling + upload fallback ---
        AVFrame* sw_frame = frame;
        AVFrame* transfer_frame = nullptr;

        if (frame->format == h->hw_pix_fmt) {
            transfer_frame = av_frame_alloc();
            if (av_hwframe_transfer_data(transfer_frame, frame, 0) < 0) {
                std::cerr << "[Decoder] av_hwframe_transfer_data failed.\n";
                av_frame_free(&transfer_frame);
                return DECODER_ERROR_DECODE_FAILED;
            }
            sw_frame = transfer_frame;
        }

        // Initialize swscale context
        if (!h->sws_ctx) {
            h->sws_ctx = sws_getContext(
                sw_frame->width, sw_frame->height, (AVPixelFormat)sw_frame->format,
                h->width, h->height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!h->sws_ctx) {
                if (transfer_frame) av_frame_free(&transfer_frame);
                return DECODER_ERROR_DECODE_FAILED;
            }
        }

        std::vector<uint8_t> rgba8(h->width * h->height * 4);
        uint8_t* dst_data[4] = { rgba8.data(), nullptr, nullptr, nullptr };
        int dst_linesize[4] = { h->width * 4, 0, 0, 0 };

        sws_scale(h->sws_ctx, sw_frame->data, sw_frame->linesize, 0, sw_frame->height, dst_data, dst_linesize);

        if (transfer_frame) {
            av_frame_free(&transfer_frame);
        }

        // Convert to RGBA32_FLOAT
        std::vector<float> rgba32f(h->width * h->height * 4);
        convert_rgba8_to_rgba32f(rgba8.data(), rgba32f.data(), h->width, h->height);

        // Upload to D3D12 DEFAULT heap buffer
        uint64_t size_in_bytes = h->width * h->height * 4 * sizeof(float);
        if (!upload_to_d3d12_texture(h, rgba32f.data(), out_texture, size_in_bytes)) {
            std::cerr << "[Decoder] Staging upload failed.\n";
            return DECODER_ERROR_DECODE_FAILED;
        }

        return DECODER_SUCCESS;
    }
}

// Fetch the next frame from the stream
static int get_next_decoded_frame(DecoderHandle* h, AVFrame* out_frame, int64_t& out_frame_idx) {
    while (true) {
        int ret = avcodec_receive_frame(h->codec_ctx, out_frame);
        if (ret == 0) {
            AVStream* stream = h->fmt_ctx->streams[h->video_stream_idx];
            int64_t pts = out_frame->pts;
            if (pts == AV_NOPTS_VALUE) {
                pts = out_frame->pkt_dts;
            }
            
            double pts_seconds = 0.0;
            if (pts != AV_NOPTS_VALUE) {
                int64_t start = stream->start_time;
                if (start == AV_NOPTS_VALUE) start = 0;
                pts_seconds = (double)(pts - start) * av_q2d(stream->time_base);
            } else {
                pts_seconds = (double)(h->current_frame + 1) * h->fps_den / h->fps_num;
            }

            out_frame_idx = (int64_t)std::round(pts_seconds * (double)h->fps_num / h->fps_den);
            return 0; // Success
        } else if (ret == AVERROR_EOF) {
            return AVERROR_EOF;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
            return ret; // Error decoding
        }

        if (h->draining) {
            return AVERROR_EOF;
        }

        // Need more packet data
        av_packet_unref(h->pkt);
        ret = av_read_frame(h->fmt_ctx, h->pkt);
        if (ret < 0) {
            avcodec_send_packet(h->codec_ctx, nullptr); // Flush stream
            h->draining = true;
            continue;
        }

        if (h->pkt->stream_index == h->video_stream_idx) {
            ret = avcodec_send_packet(h->codec_ctx, h->pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                av_packet_unref(h->pkt);
                return ret;
            }
        }
        av_packet_unref(h->pkt);
    }
}

// Check LRU cache for cached frame index
static AVFrame* find_in_cache(DecoderHandle* h, int64_t frame_index) {
    for (auto it = h->cache_list.begin(); it != h->cache_list.end(); ++it) {
        if (it->frame_index == frame_index) {
            CachedFrame item = *it;
            h->cache_list.erase(it);
            h->cache_list.push_front(item);
            return item.frame;
        }
    }
    return nullptr;
}

// Add a frame to LRU cache
static void add_to_cache(DecoderHandle* h, int64_t frame_index, AVFrame* frame) {
    for (const auto& item : h->cache_list) {
        if (item.frame_index == frame_index) return;
    }

    AVFrame* cloned = av_frame_clone(frame);
    if (!cloned) return;

    h->cache_list.push_front({frame_index, cloned});

    if (h->cache_list.size() > DecoderHandle::MAX_CACHE_SIZE) {
        AVFrame* oldest = h->cache_list.back().frame;
        av_frame_free(&oldest);
        h->cache_list.pop_back();
    }
}

// External C ABI exports

DecoderHandle* decoder_open(const char* path) {
    DecoderHandle* h = new DecoderHandle();
    h->path = path;
    h->pkt = av_packet_alloc();
    
    if (avformat_open_input(&h->fmt_ctx, path, nullptr, nullptr) < 0) {
        std::cerr << "[Decoder] Failed to open input: " << path << "\n";
        decoder_close(h);
        return nullptr;
    }

    if (avformat_find_stream_info(h->fmt_ctx, nullptr) < 0) {
        std::cerr << "[Decoder] Failed to find stream info.\n";
        decoder_close(h);
        return nullptr;
    }

    h->video_stream_idx = av_find_best_stream(h->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &h->codec, 0);
    if (h->video_stream_idx < 0) {
        std::cerr << "[Decoder] No video stream found.\n";
        decoder_close(h);
        return nullptr;
    }

    h->codec_ctx = avcodec_alloc_context3(h->codec);
    if (!h->codec_ctx) {
        decoder_close(h);
        return nullptr;
    }

    avcodec_parameters_to_context(h->codec_ctx, h->fmt_ctx->streams[h->video_stream_idx]->codecpar);

    // Default configuration for multi-threaded decoding in software fallback path
    h->codec_ctx->thread_count = 0; // Auto
    h->codec_ctx->thread_type = FF_THREAD_SLICE;

    h->width = h->codec_ctx->width;
    h->height = h->codec_ctx->height;
    
    AVStream* stream = h->fmt_ctx->streams[h->video_stream_idx];
    h->fps_num = stream->avg_frame_rate.num;
    h->fps_den = stream->avg_frame_rate.den;
    if (h->fps_num == 0 || h->fps_den == 0) {
        h->fps_num = stream->r_frame_rate.num;
        h->fps_den = stream->r_frame_rate.den;
    }
    if (h->fps_num == 0 || h->fps_den == 0) {
        h->fps_num = 30000;
        h->fps_den = 1001;
    }

    h->total_frames = stream->nb_frames;
    if (h->total_frames <= 0) {
        if (h->fmt_ctx->duration != AV_NOPTS_VALUE) {
            double duration = h->fmt_ctx->duration / (double)AV_TIME_BASE;
            h->total_frames = (int64_t)(duration * ((double)h->fps_num / h->fps_den));
        } else {
            h->total_frames = 1000;
        }
    }

    h->pending_seek_frame = -1;
    h->current_frame = -1;
    h->eof_frame = h->total_frames;

    return h;
}

ErrorCode decoder_seek(DecoderHandle* h, int64_t target_frame) {
    if (!h) return DECODER_ERROR_INVALID_ARG;
    h->pending_seek_frame = target_frame;
    if (target_frame < h->eof_frame) {
        h->draining = false;
        h->reached_eof = false;
    }
    return DECODER_SUCCESS;
}

ErrorCode decoder_decode_frame(DecoderHandle* h, TextureHandle* out_texture) {
    if (!h || !out_texture) return DECODER_ERROR_INVALID_ARG;

    // Retrieve D3D12 Device from the target texture
    ID3D12Device* d3d12_device = nullptr;
    HRESULT hr = out_texture->GetDevice(IID_PPV_ARGS(&d3d12_device));
    if (FAILED(hr) || !d3d12_device) {
        return DECODER_ERROR_INVALID_ARG;
    }
    // Decrement the reference count since GetDevice increments it
    d3d12_device->Release();

    if (!ensure_initialized(h, d3d12_device)) {
        return DECODER_ERROR_HW_INIT_FAILED;
    }

    int64_t target_frame = h->current_frame + 1;
    if (h->pending_seek_frame != -1) {
        target_frame = h->pending_seek_frame;
        h->pending_seek_frame = -1;
    }

    if (target_frame < 0) {
        target_frame = 0;
    }

    if (target_frame >= h->eof_frame) {
        return DECODER_ERROR_EOF;
    }

    // 1. Check LRU Cache
    AVFrame* cached_frame = find_in_cache(h, target_frame);
    if (cached_frame) {
        h->current_frame = target_frame;
        return output_frame_to_texture(h, cached_frame, out_texture);
    }

    // 2. Perform seek if target_frame is behind, or too far ahead
    if (target_frame < h->current_frame || target_frame > h->current_frame + 30) {
        AVStream* stream = h->fmt_ctx->streams[h->video_stream_idx];
        double time_in_seconds = (double)target_frame * h->fps_den / h->fps_num;
        int64_t target_pts = (int64_t)(time_in_seconds / av_q2d(stream->time_base));

        int seek_ret = av_seek_frame(h->fmt_ctx, h->video_stream_idx, target_pts, AVSEEK_FLAG_BACKWARD);
        if (seek_ret < 0) {
            std::cerr << "[Decoder] av_seek_frame failed: " << seek_ret << "\n";
            return DECODER_ERROR_SEEK_FAILED;
        }

        avcodec_flush_buffers(h->codec_ctx);
        h->current_frame = -1;
        if (target_frame < h->eof_frame) {
            h->draining = false;
            h->reached_eof = false;
        }
    }

    // 3. Forward decode and fill cache
    AVFrame* temp_frame = av_frame_alloc();
    if (!temp_frame) {
        return DECODER_ERROR_DECODE_FAILED;
    }

    bool found = false;
    int max_attempts = 300; 
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        int64_t decoded_frame_idx = -1;
        int ret = get_next_decoded_frame(h, temp_frame, decoded_frame_idx);
        if (ret < 0) {
            av_frame_free(&temp_frame);
            if (ret == AVERROR_EOF) {
                h->reached_eof = true;
                h->eof_frame = target_frame;
                return DECODER_ERROR_EOF;
            }
            return DECODER_ERROR_DECODE_FAILED;
        }

        h->current_frame = decoded_frame_idx;
        add_to_cache(h, decoded_frame_idx, temp_frame);

        if (decoded_frame_idx == target_frame) {
            found = true;
            break;
        }

        if (decoded_frame_idx > target_frame) {
            found = true;
            break;
        }

        av_frame_unref(temp_frame);
    }

    if (!found) {
        av_frame_free(&temp_frame);
        return DECODER_ERROR_DECODE_FAILED;
    }

    AVFrame* final_frame = find_in_cache(h, h->current_frame);
    av_frame_free(&temp_frame);

    if (!final_frame) {
        return DECODER_ERROR_DECODE_FAILED;
    }

    return output_frame_to_texture(h, final_frame, out_texture);
}

int32_t decoder_decode_audio_frame(
    DecoderHandle* h,
    int64_t source_frame,
    float* out_pcm,
    int32_t* out_sample_count,
    uint32_t target_sample_rate
) {
    (void)target_sample_rate;

    if (!h || !out_pcm || !out_sample_count) return -1;
    if (*out_sample_count <= 0) return -1;

    // 1. Initialize audio codec context and stream info ONCE
    if (h->audio_stream_idx < 0) {
        h->audio_stream_idx = av_find_best_stream(h->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (h->audio_stream_idx < 0) {
            *out_sample_count = 0;
            return -1; // No audio stream
        }

        AVStream* audio_stream = h->fmt_ctx->streams[h->audio_stream_idx];
        h->audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (!h->audio_codec) {
            h->audio_stream_idx = -1;
            *out_sample_count = 0;
            return -1;
        }

        h->audio_ctx = avcodec_alloc_context3(h->audio_codec);
        if (!h->audio_ctx) {
            h->audio_stream_idx = -1;
            *out_sample_count = 0;
            return -1;
        }

        if (avcodec_parameters_to_context(h->audio_ctx, audio_stream->codecpar) < 0) {
            avcodec_free_context(&h->audio_ctx);
            h->audio_stream_idx = -1;
            *out_sample_count = 0;
            return -1;
        }

        // Set slice threading for audio context to optimize performance safely
        h->audio_ctx->thread_count = 0; // Auto
        h->audio_ctx->thread_type = FF_THREAD_SLICE;

        if (avcodec_open2(h->audio_ctx, h->audio_codec, nullptr) < 0) {
            avcodec_free_context(&h->audio_ctx);
            h->audio_stream_idx = -1;
            *out_sample_count = 0;
            return -1;
        }
        h->audio_pkt = av_packet_alloc();
        h->audio_frame = av_frame_alloc();
        h->current_audio_frame = -1;
        h->current_audio_sample = -1;
    }

    double fps = (h->fps_num > 0 && h->fps_den > 0)
        ? (double)h->fps_num / h->fps_den : 30.0;
    double sample_rate = h->audio_ctx->sample_rate;
    int64_t requested_sample = (int64_t)(source_frame * sample_rate / fps);

    bool need_seek = false;
    if (h->current_audio_sample == -1) {
        need_seek = true;
    } else if (source_frame < h->current_audio_frame) {
        need_seek = true;  // scrubbed backward or looped
    } else if (source_frame > h->current_audio_frame + 60) {
        need_seek = true;  // jumped forward more than 2 seconds
    }

    if (need_seek) {
        double time_in_seconds = (double)source_frame / fps;
        int64_t seek_ts = (int64_t)(time_in_seconds * AV_TIME_BASE);
        av_seek_frame(h->fmt_ctx, -1, seek_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(h->audio_ctx);
        h->current_audio_frame = source_frame;
        h->current_audio_sample = requested_sample;
    } else {
        h->current_audio_frame = source_frame;
    }

    int32_t max_pairs = *out_sample_count; // capacity in stereo pairs
    int32_t written_pairs = 0;

    int src_channels = h->audio_ctx->ch_layout.nb_channels;
    if (src_channels < 1) src_channels = 1;

    // 3. Read and decode packets
    while (written_pairs < max_pairs) {
        int ret = av_read_frame(h->fmt_ctx, h->audio_pkt);
        if (ret < 0) break; // EOF or error

        if (h->audio_pkt->stream_index != h->audio_stream_idx) {
            av_packet_unref(h->audio_pkt);
            continue;
        }

        ret = avcodec_send_packet(h->audio_ctx, h->audio_pkt);
        av_packet_unref(h->audio_pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(h->audio_ctx, h->audio_frame) == 0 && written_pairs < max_pairs) {
            int nb = h->audio_frame->nb_samples;
            AVSampleFormat fmt = (AVSampleFormat)h->audio_frame->format;
            bool is_planar = av_sample_fmt_is_planar(fmt) != 0;

            int64_t pts = h->audio_frame->pts;
            if (pts == AV_NOPTS_VALUE) {
                pts = h->audio_frame->pkt_dts;
            }

            int start_s = 0;
            if (pts != AV_NOPTS_VALUE) {
                int64_t frame_sample_idx = av_rescale_q(
                    pts,
                    h->fmt_ctx->streams[h->audio_stream_idx]->time_base,
                    AVRational{1, h->audio_ctx->sample_rate}
                );

                if (frame_sample_idx + nb <= requested_sample) {
                    // Entire frame is before the requested start time - discard
                    av_frame_unref(h->audio_frame);
                    continue;
                }
                if (frame_sample_idx < requested_sample) {
                    // Requested start time lies inside this frame
                    start_s = (int)(requested_sample - frame_sample_idx);
                }
            }

            for (int s = start_s; s < nb && written_pairs < max_pairs; ++s) {
                float L = 0.0f, R = 0.0f;

                auto get_sample = [&](int ch) -> float {
                    if (ch >= src_channels) ch = src_channels - 1;
                    uint8_t* data = is_planar ? h->audio_frame->data[ch] : h->audio_frame->data[0];
                    int idx = is_planar ? s : (s * src_channels + ch);

                    switch (fmt) {
                        case AV_SAMPLE_FMT_FLTP:
                        case AV_SAMPLE_FMT_FLT:
                            return reinterpret_cast<float*>(data)[idx];
                        case AV_SAMPLE_FMT_DBLP:
                        case AV_SAMPLE_FMT_DBL:
                            return (float)reinterpret_cast<double*>(data)[idx];
                        case AV_SAMPLE_FMT_S16P:
                        case AV_SAMPLE_FMT_S16:
                            return reinterpret_cast<int16_t*>(data)[idx] / 32768.0f;
                        case AV_SAMPLE_FMT_S32P:
                        case AV_SAMPLE_FMT_S32:
                            return reinterpret_cast<int32_t*>(data)[idx] / 2147483648.0f;
                        case AV_SAMPLE_FMT_U8P:
                        case AV_SAMPLE_FMT_U8:
                            return (reinterpret_cast<uint8_t*>(data)[idx] - 128) / 128.0f;
                        default:
                            return 0.0f;
                    }
                };

                L = get_sample(0);
                R = (src_channels >= 2) ? get_sample(1) : L;

                out_pcm[written_pairs * 2 + 0] = L;
                out_pcm[written_pairs * 2 + 1] = R;
                ++written_pairs;
            }

            av_frame_unref(h->audio_frame);
        }
    }

    *out_sample_count = written_pairs;

    h->current_audio_sample += written_pairs;
    h->current_audio_frame = (int64_t)(h->current_audio_sample * fps
                               / h->audio_ctx->sample_rate);

    return (written_pairs > 0) ? 0 : -1;
}



AudioInfo decoder_get_audio_info(DecoderHandle* h) {
    AudioInfo info = {};
    if (!h || !h->fmt_ctx) return info;

    int audio_stream_idx = av_find_best_stream(h->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_idx < 0) return info;

    AVStream* audio_stream = h->fmt_ctx->streams[audio_stream_idx];
    info.sample_rate = audio_stream->codecpar->sample_rate;
    info.channels    = audio_stream->codecpar->ch_layout.nb_channels;
    if (info.channels < 1) info.channels = 1;
    return info;
}

MediaInfo decoder_get_info(DecoderHandle* h) {
    MediaInfo info = {};
    if (h) {
        info.width = h->width;
        info.height = h->height;
        info.fps_num = h->fps_num;
        info.fps_den = h->fps_den;
        info.total_frames = h->total_frames;
    }
    return info;
}

void decoder_close(DecoderHandle* h) {
    if (!h) return;

    for (auto& item : h->cache_list) {
        if (item.frame) {
            av_frame_free(&item.frame);
        }
    }
    h->cache_list.clear();

    if (h->sws_ctx) {
        sws_freeContext(h->sws_ctx);
    }
    if (h->codec_ctx) {
        avcodec_free_context(&h->codec_ctx);
    }
    if (h->fmt_ctx) {
        avformat_close_input(&h->fmt_ctx);
    }
    if (h->hw_device_ctx) {
        av_buffer_unref(&h->hw_device_ctx);
    }
    if (h->pkt) {
        av_packet_free(&h->pkt);
    }

    if (h->audio_ctx) {
        avcodec_free_context(&h->audio_ctx);
    }
    if (h->audio_pkt) {
        av_packet_free(&h->audio_pkt);
    }
    if (h->audio_frame) {
        av_frame_free(&h->audio_frame);
    }

    if (h->fence_event) {
        CloseHandle(h->fence_event);
    }

    delete h;
}
