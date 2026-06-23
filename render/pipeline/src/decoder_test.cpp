#include "video_decoder.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

#include "texture_pool.h"

int main(int argc, char* argv[]) {
    std::cout << "=== Palmier Video Decoder Pipeline Test ===" << std::endl;

    const char* video_path = (argc > 1) ? argv[1] : "sample.mp4";
    std::cout << "[Test] Video path: " << video_path << std::endl;

    // 1. Initialize DXGI and D3D12 Device
    ComPtr<IDXGIFactory4> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        std::cerr << "[Test] Failed to create DXGI Factory: 0x" << std::hex << hr << std::dec << std::endl;
        return 1;
    }

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::cerr << "[Test] Failed to create D3D12 Device: 0x" << std::hex << hr << std::dec << std::endl;
        return 1;
    }
    std::cout << "[Test] D3D12 Device created." << std::endl;

    // 2. Open Decoder
    DecoderHandle* decoder = decoder_open(video_path);
    if (!decoder) {
        std::cerr << "[Test] Failed to open decoder for: " << video_path << std::endl;
        std::cerr << "[Test] Make sure the file exists and is a valid video." << std::endl;
        return 1;
    }
    std::cout << "[Test] Decoder opened successfully." << std::endl;

    MediaInfo info = decoder_get_info(decoder);
    std::cout << "[Test] Media Details:" << std::endl;
    std::cout << "  Resolution: " << info.width << "x" << info.height << std::endl;
    std::cout << "  FPS: " << info.fps_num << "/" << info.fps_den 
              << " (" << (double)info.fps_num / info.fps_den << ")" << std::endl;
    std::cout << "  Total Frames: " << info.total_frames << std::endl;

    if (info.width <= 0 || info.height <= 0) {
        std::cerr << "[Test] Invalid media resolution." << std::endl;
        decoder_close(decoder);
        return 1;
    }

    // 3. Initialize TexturePool matching the video dimensions
    std::cout << "[Test] Allocating TexturePool of size " << info.width << "x" << info.height << "..." << std::endl;
    TexturePool pool(device.Get(), info.width, info.height, 4);
    
    int tex_idx = pool.acquire();
    if (tex_idx < 0) {
        std::cerr << "[Test] Failed to acquire texture from pool." << std::endl;
        decoder_close(decoder);
        return 1;
    }
    ID3D12Resource* out_texture = pool.get_resource(tex_idx);
    std::cout << "[Test] Texture acquired from pool successfully." << std::endl;

    // 4. Seek to frame 30
    int64_t target_frame = 30;
    std::cout << "[Test] Seeking to frame " << target_frame << "..." << std::endl;
    ErrorCode err = decoder_seek(decoder, target_frame);
    if (err != DECODER_SUCCESS) {
        std::cerr << "[Test] decoder_seek failed: " << err << std::endl;
        pool.release(tex_idx);
        decoder_close(decoder);
        return 1;
    }

    // 5. Decode frame
    std::cout << "[Test] Decoding frame " << target_frame << " into GPU texture..." << std::endl;
    err = decoder_decode_frame(decoder, out_texture);
    if (err != DECODER_SUCCESS) {
        std::cerr << "[Test] decoder_decode_frame failed: " << err << std::endl;
        pool.release(tex_idx);
        decoder_close(decoder);
        return 1;
    }
    std::cout << "[Test] SUCCESS: Frame 30 decoded directly to GPU texture." << std::endl;

    // 6. Cleanup
    pool.release(tex_idx);
    decoder_close(decoder);
    std::cout << "[Test] Decoder closed. Test passed successfully." << std::endl;

    return 0;
}
