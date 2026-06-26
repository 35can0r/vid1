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

// Video metadata information
typedef struct MediaInfo {
    int32_t width;
    int32_t height;
    int32_t fps_num;
    int32_t fps_den;
    int64_t total_frames;
} MediaInfo;

// Audio metadata: native sample rate and channel count (returned by decoder_get_audio_info)
typedef struct AudioInfo {
    int32_t sample_rate;  // e.g. 44100 or 48000
    int32_t channels;     // 1 = mono, 2 = stereo, etc.
} AudioInfo;

// C ABI Exported Functions
DecoderHandle* decoder_open(const char* path);
ErrorCode decoder_seek(DecoderHandle* h, int64_t target_frame);
ErrorCode decoder_decode_frame(DecoderHandle* h, TextureHandle* out_texture);

// Query audio stream metadata (native sample_rate, channels). Returns {0,0} if no audio stream.
AudioInfo decoder_get_audio_info(DecoderHandle* h);

// Audio decode: seek to source_frame, decode up to *out_sample_count stereo f32 sample-pairs
// into out_pcm (interleaved L/R). PCM is at the native sample rate (use decoder_get_audio_info).
// out_pcm must be pre-allocated to at least (*out_sample_count * 2) floats.
// Sets *out_sample_count to the actual number of stereo sample-pairs written.
// Returns 0 on success, negative on error/EOF.
int32_t decoder_decode_audio_frame(
    DecoderHandle* h,
    int64_t source_frame,
    float* out_pcm,
    int32_t* out_sample_count,
    uint32_t target_sample_rate  // currently unused; resampling done in Rust
);

MediaInfo decoder_get_info(DecoderHandle* h);
void decoder_close(DecoderHandle* h);

#ifdef __cplusplus
}
#endif
