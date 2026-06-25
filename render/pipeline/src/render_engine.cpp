#include "../include/renderer.h"
#include <d3d12.h>
#include <wrl/client.h>
#include "../../effects/src/texture_pool.h"
#include "../include/compositor.h"
#include "video_decoder.h"
#include <iostream>
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

struct ActiveClipC {
    uint8_t clip_id[37];
    uint8_t media_ref[37];
    int64_t source_frame;
    uint32_t track_index;
    float center_x;
    float center_y;
    float width;
    float height;
    float rotation;
    float crop_left;
    float crop_top;
    float crop_right;
    float crop_bottom;
    float exposure;
    float contrast;
    float temperature;
    float tint;
    float saturation;
    float opacity;
};

struct RendererHandle {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    TexturePool* pool;
    CompositorHandle compositor;
    std::unordered_map<std::string, DecoderHandle*> decoder_cache;
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
        uint32_t pool_w = (std::max)(canvas_width,  1920u);
        uint32_t pool_h = (std::max)(canvas_height, 1080u);
        r->pool = new TexturePool(r->device.Get(), pool_w, pool_h, 8);

        // 4. Initialize the Compositor
        r->compositor = compositor_create(r->device.Get(), r->queue.Get(), pool_w, pool_h);

        return r;
    }

    void renderer_destroy(RendererHandle* r) {
        if (r) {
            for (auto& pair : r->decoder_cache) {
                if (pair.second) decoder_close(pair.second);
            }
            r->decoder_cache.clear();
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

    TextureHandle* render_frame(RendererHandle* r, void* timeline, int64_t frame_number) {
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

        CompositeResult result = {};

        ActiveClipC* active_clips = nullptr;
        int32_t count = 0;

        if (timeline) {
            count = timeline_get_active_clips(timeline, frame_number, &active_clips);
        }

        if (count > 0 && active_clips) {
            std::vector<LayerDesc> layers;
            std::vector<int> acquired_slots;

            for (int32_t i = 0; i < count; ++i) {
                ActiveClipC* clip = active_clips + i;

                std::string media_ref_str((char*)clip->media_ref);
                DecoderHandle* dec = nullptr;

                auto it = r->decoder_cache.find(media_ref_str);
                if (it != r->decoder_cache.end()) {
                    dec = it->second;
                } else {
                    char* resolved_path = timeline_resolve_media((const char*)clip->media_ref);
                    if (resolved_path) {
                        dec = decoder_open(resolved_path);
                        if (!dec) dec = decoder_open((std::string("../") + resolved_path).c_str());
                        if (!dec) dec = decoder_open((std::string("../../") + resolved_path).c_str());
                        if (!dec) dec = decoder_open((std::string("../../../") + resolved_path).c_str());
                        if (dec) {
                            r->decoder_cache[media_ref_str] = dec;
                        }
                        string_free(resolved_path);
                    }
                }

                if (dec) {
                    int layer_slot = r->pool->acquire();
                    if (layer_slot >= 0) {
                        TextureHandle* layer_texture = r->pool->get_resource(layer_slot);
                        decoder_seek(dec, clip->source_frame);
                        decoder_decode_frame(dec, layer_texture);
                        acquired_slots.push_back(layer_slot);

                        MediaInfo info = decoder_get_info(dec);
                        uint32_t pool_w = r->pool->GetWidth();
                        uint32_t pool_h = r->pool->GetHeight();
                        float scale_x = (float)info.width / pool_w;
                        float scale_y = (float)info.height / pool_h;

                        LayerDesc layer = {};
                        layer.texture = layer_texture;
                        layer.opacity = clip->opacity;
                        layer.transform.center_x = clip->center_x;
                        layer.transform.center_y = clip->center_y;
                        layer.transform.width = clip->width;
                        layer.transform.height = clip->height;
                        layer.transform.rotation = clip->rotation;

                        layer.crop.left = clip->crop_left * scale_x;
                        layer.crop.top = clip->crop_top * scale_y;
                        layer.crop.right = 1.0f - (1.0f - clip->crop_right) * scale_x;
                        layer.crop.bottom = 1.0f - (1.0f - clip->crop_bottom) * scale_y;
                        layer.grade = { clip->exposure, clip->contrast, clip->temperature, clip->tint, clip->saturation, {0,0,0} };

                        layers.push_back(layer);
                    }
                }
            }

            compositor_composite(r->compositor, layers.data(), layers.size(), output_texture);

            for (int s : acquired_slots) {
                r->pool->release(s);
            }

            active_clips_free(active_clips, count);
        } else {
            compositor_composite(r->compositor, nullptr, 0, output_texture);
        }

        return output_texture;
    }
}
