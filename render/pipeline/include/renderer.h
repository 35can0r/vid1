#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct PresenterHandle PresenterHandle;
typedef struct RendererHandle RendererHandle;
typedef struct ID3D12Resource TextureHandle;

// Engine API
RendererHandle* renderer_create(uint32_t canvas_width, uint32_t canvas_height);
void renderer_destroy(RendererHandle* r);

#ifdef __cplusplus
struct ID3D12Device;
struct ID3D12CommandQueue;
ID3D12Device* render_engine_get_device(RendererHandle* r);
ID3D12CommandQueue* render_engine_get_queue(RendererHandle* r);
#endif

// Presenter API
PresenterHandle* presenter_create(void* swap_chain_panel_native, RendererHandle* engine, uint32_t width, uint32_t height);
void presenter_present(PresenterHandle* presenter, TextureHandle* compositor_output);
void presenter_resize(PresenterHandle* presenter, uint32_t width, uint32_t height);
void presenter_destroy(PresenterHandle* presenter);

// Render Engine API
TextureHandle* render_frame(RendererHandle* r, int64_t frame_number);

#ifdef __cplusplus
}
#endif
