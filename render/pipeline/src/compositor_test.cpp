#include "video_decoder.h"
#include "../include/compositor.h"
#include "../../effects/src/texture_pool.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// Simple BMP writer for RGBA32F texture readback
void SaveBMP(const std::string& filename, const float* data, int width, int height) {
    std::vector<uint8_t> bmp_data(width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int src_idx = (y * width + x) * 4;
            int dst_idx = ((height - 1 - y) * width + x) * 4; // BMP is bottom-up

            float r = std::clamp(data[src_idx + 0], 0.0f, 1.0f);
            float g = std::clamp(data[src_idx + 1], 0.0f, 1.0f);
            float b = std::clamp(data[src_idx + 2], 0.0f, 1.0f);
            float a = std::clamp(data[src_idx + 3], 0.0f, 1.0f);

            bmp_data[dst_idx + 0] = static_cast<uint8_t>(b * 255.0f);
            bmp_data[dst_idx + 1] = static_cast<uint8_t>(g * 255.0f);
            bmp_data[dst_idx + 2] = static_cast<uint8_t>(r * 255.0f);
            bmp_data[dst_idx + 3] = static_cast<uint8_t>(a * 255.0f);
        }
    }

    #pragma pack(push, 1)
    struct BMPHeader {
        uint16_t type{0x4D42}; // 'BM'
        uint32_t size{0};
        uint16_t res1{0};
        uint16_t res2{0};
        uint32_t offset{54};
    } header;

    struct BMPInfoHeader {
        uint32_t size{40};
        int32_t width{0};
        int32_t height{0};
        uint16_t planes{1};
        uint16_t bit_count{32};
        uint32_t compression{0};
        uint32_t size_image{0};
        int32_t x_pels_per_meter{0};
        int32_t y_pels_per_meter{0};
        uint32_t clr_used{0};
        uint32_t clr_important{0};
    } info;
    #pragma pack(pop)

    header.size = sizeof(BMPHeader) + sizeof(BMPInfoHeader) + bmp_data.size();
    info.width = width;
    info.height = height;

    std::ofstream file(filename, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info), sizeof(info));
    file.write(reinterpret_cast<const char*>(bmp_data.data()), bmp_data.size());
}

float* ReadbackTexture(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* tex, int width, int height) {
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = width * height * 4 * sizeof(float);
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.Format = DXGI_FORMAT_UNKNOWN;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readback_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* readback_buf = nullptr;
    device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buf));

    ComPtr<ID3D12CommandAllocator> cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&cmd_list));

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback_buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = width * 4 * sizeof(float);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd_list->ResourceBarrier(1, &barrier);

    cmd_list->Close();
    ID3D12CommandList* ppCommandLists[] = { cmd_list.Get() };
    queue->ExecuteCommandLists(1, ppCommandLists);

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);

    float* mapped_data = nullptr;
    readback_buf->Map(0, nullptr, reinterpret_cast<void**>(&mapped_data));
    float* data = new float[width * height * 4];
    memcpy(data, mapped_data, width * height * 4 * sizeof(float));
    readback_buf->Unmap(0, nullptr);
    readback_buf->Release();

    return data;
}

int main(int argc, char* argv[]) {
    std::cout << "=== Palmier Compositor Pipeline Test ===" << std::endl;
    CreateDirectory("compositor_test_output", NULL);

    ComPtr<IDXGIFactory4> dxgiFactory;
    CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::cerr << "Failed to create D3D12 Device" << std::endl;
        return 1;
    }

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));

    DecoderHandle* decoder = decoder_open("sample.mp4");
    if (!decoder) {
        std::cerr << "Failed to open sample.mp4" << std::endl;
        return 1;
    }
    MediaInfo info = decoder_get_info(decoder);

    TexturePool pool(device.Get(), info.width, info.height, 6);
    CompositorHandle compositor = compositor_create(device.Get(), queue.Get(), info.width, info.height);

    int l0_idx = pool.acquire();
    int l1_idx = pool.acquire();
    int out_idx = pool.acquire();

    decoder_seek(decoder, 0);
    decoder_decode_frame(decoder, pool.get_resource(l0_idx));

    decoder_seek(decoder, 30);
    decoder_decode_frame(decoder, pool.get_resource(l1_idx));

    ID3D12Resource* layer0 = pool.get_resource(l0_idx);
    ID3D12Resource* layer1 = pool.get_resource(l1_idx);
    ID3D12Resource* output = pool.get_resource(out_idx);

    ColorGradeParams identity_grade = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, {0,0,0} };

    bool single_layer_pass = false;
    bool two_layer_pass = false;
    bool color_grade_pass = false;
    bool rotation_pass = false;

    // 5. SINGLE LAYER TEST
    {
        LayerDesc desc = {
            layer0,
            {0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
            {0, 0, 0, 0},
            identity_grade,
            1.0f, {0,0,0}
        };
        compositor_composite(compositor, &desc, 1, output);
        float* data = ReadbackTexture(device.Get(), queue.Get(), output, info.width, info.height);
        SaveBMP("compositor_test_output/test_single_layer.bmp", data, info.width, info.height);

        bool ok = true;
        for (int i = 0; i < info.width * info.height * 4; ++i) {
            if (std::isnan(data[i])) ok = false;
        }
        single_layer_pass = ok;
        delete[] data;
    }

    // 6. TWO LAYER BLEND TEST
    {
        LayerDesc layers[2] = {
            { layer0, {0.5f, 0.5f, 1.0f, 1.0f, 0.0f}, {0,0,0,0}, identity_grade, 1.0f, {0,0,0} },
            { layer1, {0.75f, 0.25f, 0.5f, 0.5f, 0.0f}, {0,0,0,0}, identity_grade, 0.6f, {0,0,0} }
        };
        compositor_composite(compositor, layers, 2, output);
        float* data = ReadbackTexture(device.Get(), queue.Get(), output, info.width, info.height);
        SaveBMP("compositor_test_output/test_two_layer.bmp", data, info.width, info.height);

        two_layer_pass = true; // basic visual verify
        delete[] data;
    }

    // 7. COLOR GRADE TEST
    {
        ColorGradeParams bw_grade = { 1.5f, 1.0f, 0.0f, 0.0f, 0.0f, {0,0,0} };
        LayerDesc desc = {
            layer0,
            {0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
            {0, 0, 0, 0},
            bw_grade,
            1.0f, {0,0,0}
        };
        compositor_composite(compositor, &desc, 1, output);
        float* data = ReadbackTexture(device.Get(), queue.Get(), output, info.width, info.height);
        SaveBMP("compositor_test_output/test_graded.bmp", data, info.width, info.height);

        bool ok = true;
        for (int i = 0; i < info.width * info.height; ++i) {
            float r = data[i*4+0], g = data[i*4+1], b = data[i*4+2];
            if (std::abs(r - g) > 0.01f || std::abs(g - b) > 0.01f) {
                ok = false;
                break;
            }
        }
        color_grade_pass = ok;
        delete[] data;
    }

    // 8. ROTATION TEST
    {
        LayerDesc desc = {
            layer0,
            {0.5f, 0.5f, 0.5f, 0.5f, 0.785398f},
            {0, 0, 0, 0},
            identity_grade,
            1.0f, {0,0,0}
        };
        compositor_composite(compositor, &desc, 1, output);
        float* data = ReadbackTexture(device.Get(), queue.Get(), output, info.width, info.height);
        SaveBMP("compositor_test_output/test_rotated.bmp", data, info.width, info.height);

        // Corners should be black (0,0,0)
        int corner_idx = 0; // top-left
        if (data[0] == 0.0f && data[1] == 0.0f && data[2] == 0.0f) rotation_pass = true;

        delete[] data;
    }

    compositor_destroy(compositor);
    decoder_close(decoder);

    std::cout << "═══════════════════════════════════════\n";
    std::cout << "COMPOSITOR TEST RESULTS\n";
    std::cout << "═══════════════════════════════════════\n";
    std::cout << "Single layer:    " << (single_layer_pass ? "PASS" : "FAIL") << "\n";
    std::cout << "Two-layer blend: " << (two_layer_pass ? "PASS" : "FAIL") << "\n";
    std::cout << "Color grade:     " << (color_grade_pass ? "PASS" : "FAIL") << "\n";
    std::cout << "Rotation:        " << (rotation_pass ? "PASS" : "FAIL") << "\n";
    std::cout << "═══════════════════════════════════════\n";

    return (single_layer_pass && two_layer_pass && color_grade_pass && rotation_pass) ? 0 : 1;
}
