#include "../include/renderer.h"
#include <d3d12.h>
#include <wrl/client.h>
#include "../../effects/src/texture_pool.h"
#include "../include/compositor.h"
#include "video_decoder.h"
#include <iostream>

using Microsoft::WRL::ComPtr;

struct RendererHandle {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    TexturePool* pool;
    CompositorHandle compositor;
    DecoderHandle* decoder = nullptr;
    uint32_t width;
    uint32_t height;
    int last_slot = -1;
};

extern "C" {
    RendererHandle* renderer_create(uint32_t canvas_width, uint32_t canvas_height) {
        auto* r = new RendererHandle();
        r->width = canvas_width;
        r->height = canvas_height;
        r->pool = nullptr;
        r->compositor = nullptr;
        r->last_slot = -1;

        // 1. Initialize DX12 Device
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r->device)))) {
            delete r;
            return nullptr;
        }

        // 2. Initialize Command Queue
        D3D12_COMMAND_QUEUE_DESC qDesc = {};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(r->device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&r->queue)))) {
            delete r;
            return nullptr;
        }

        // 3. Initialize the VRAM TexturePool
        r->pool = new TexturePool(r->device.Get(), canvas_width, canvas_height);

        // 4. Initialize the Compositor
        r->compositor = compositor_create(r->device.Get(), r->queue.Get(), canvas_width, canvas_height);

        // 5. Initialize the Video Decoder with fallback paths to handle working directory mismatches
        r->decoder = decoder_open("sample.mp4");
        if (!r->decoder) r->decoder = decoder_open("../sample.mp4");
        if (!r->decoder) r->decoder = decoder_open("../../sample.mp4");
        if (!r->decoder) r->decoder = decoder_open("../../../sample.mp4");
        if (!r->decoder) r->decoder = decoder_open("../../../../sample.mp4");

        if (r->decoder) {
            std::cout << "[RenderEngine] Loaded sample.mp4 successfully for preview." << std::endl;
        } else {
            std::cerr << "[RenderEngine] Warning: Failed to locate sample.mp4." << std::endl;
        }

        return r;
    }

    void renderer_destroy(RendererHandle* r) {
        if (r) {
            if (r->decoder) decoder_close(r->decoder);
            if (r->compositor) compositor_destroy(r->compositor);
            if (r->pool) delete r->pool;
            delete r;
        }
    }

    ID3D12Device* render_engine_get_device(RendererHandle* r) {
        return r ? r->device.Get() : nullptr;
    }

    ID3D12CommandQueue* render_engine_get_queue(RendererHandle* r) {
        return r ? r->queue.Get() : nullptr;
    }

    TextureHandle* render_frame(RendererHandle* r, int64_t frame_number) {
        if (!r || !r->compositor || !r->pool) return nullptr;

        // Release the previous frame's slot to prevent pool exhaustion
        if (r->last_slot != -1) {
            r->pool->release(r->last_slot);
            r->last_slot = -1;
        }

        // Get an output texture from the pool
        int slot = r->pool->acquire();
        if (slot < 0) {
            return nullptr;
        }
        r->last_slot = slot;
        TextureHandle* output_texture = r->pool->get_resource(slot);

        // Decode frame if decoder is active
        TextureHandle* layer_texture = nullptr;
        int layer_slot = -1;
        if (r->decoder) {
            layer_slot = r->pool->acquire();
            if (layer_slot >= 0) {
                layer_texture = r->pool->get_resource(layer_slot);
                decoder_seek(r->decoder, frame_number);
                decoder_decode_frame(r->decoder, layer_texture);
            }
        }

        CompositeResult result = {};
        if (layer_texture) {
            LayerDesc layer = {};
            layer.texture = layer_texture;
            layer.opacity = 1.0f;
            layer.transform.center_x = 0.5f;
            layer.transform.center_y = 0.5f;
            layer.transform.width = 1.0f;
            layer.transform.height = 1.0f;
            layer.transform.rotation = 0.0f;
            layer.crop = { 0.0f, 0.0f, 0.0f, 0.0f };
            layer.grade = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, {0,0,0} };

            result = compositor_composite_async(r->compositor, &layer, 1, output_texture);
        } else {
            result = compositor_composite_async(r->compositor, nullptr, 0, output_texture);
        }

        // Wait on fence
        if (result.fence && result.fence->GetCompletedValue() < result.fence_value) {
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (event) {
                result.fence->SetEventOnCompletion(result.fence_value, event);
                WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
            }
        }

        // Release the temporary slot back to the pool immediately after rendering
        if (layer_slot >= 0) {
            r->pool->release(layer_slot);
        }

        return result.output;
    }
}
