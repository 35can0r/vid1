#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct DecoderHandle DecoderHandle;
typedef struct ID3D12Resource TextureHandle;

// Error codes for the decode pipeline
typedef enum ErrorCode {
    DECODER_SUCCESS = 0,
    DECODER_ERROR_EOF = -1,
    DECODER_ERROR_SEEK_FAILED = -2,
    DECODER_ERROR_DECODE_FAILED = -3,
    DECODER_ERROR_INVALID_ARG = -4,
    DECODER_ERROR_HW_INIT_FAILED = -5
} ErrorCode;

// Media metadata information
typedef struct MediaInfo {
    int32_t width;
    int32_t height;
    int32_t fps_num;
    int32_t fps_den;
    int64_t total_frames;
} MediaInfo;

// C ABI Exported Functions
DecoderHandle* decoder_open(const char* path);
ErrorCode decoder_seek(DecoderHandle* h, int64_t target_frame);
ErrorCode decoder_decode_frame(DecoderHandle* h, TextureHandle* out_texture);
MediaInfo decoder_get_info(DecoderHandle* h);
void decoder_close(DecoderHandle* h);

#ifdef __cplusplus
}
#endif
