#pragma once

#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

// Represent the output software buffer
struct VideoFrame {
    int width;
    int height;
    int linesize;
    std::vector<uint8_t> data;
    int64_t pts;
};

class Decoder {
public:
    Decoder();
    ~Decoder();

    bool open(const std::string& path);
    bool seek_to_frame(int64_t target_frame, double fps);
    bool read_next_frame(VideoFrame* out);

private:
    void cleanup();
    bool init_hw_decoder(AVCodecContext* ctx, const enum AVHWDeviceType type);
    static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);

    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    int video_stream_idx;

    AVBufferRef* hw_device_ctx;
    AVPixelFormat hw_pix_fmt;
    bool hw_active;

    AVFrame* frame;
    AVFrame* sw_frame;
    AVPacket* pkt;

    SwsContext* sws_ctx;

    // Cache the device format we want to initialize (DXVA2 or D3D11VA)
    AVHWDeviceType requested_hw_type;
};
