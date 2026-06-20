#include "decoder.h"
#include <iostream>

Decoder::Decoder()
    : fmt_ctx(nullptr), codec_ctx(nullptr), video_stream_idx(-1),
      hw_device_ctx(nullptr), hw_pix_fmt(AV_PIX_FMT_NONE), hw_active(false),
      frame(nullptr), sw_frame(nullptr), pkt(nullptr), sws_ctx(nullptr),
      requested_hw_type(AV_HWDEVICE_TYPE_D3D11VA) // Windows ARM64 target
{
    frame = av_frame_alloc();
    sw_frame = av_frame_alloc();
    pkt = av_packet_alloc();
}

Decoder::~Decoder() {
    cleanup();
}

void Decoder::cleanup() {
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
    if (frame) av_frame_free(&frame);
    if (sw_frame) av_frame_free(&sw_frame);
    if (pkt) av_packet_free(&pkt);
}

enum AVPixelFormat Decoder::get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == (enum AVPixelFormat)(intptr_t)ctx->opaque) { // We pass the expected hw_pix_fmt through opaque
            return *p;
        }
    }
    std::cerr << "Failed to get HW surface format. Falling back to software.\n";
    return AV_PIX_FMT_NONE;
}

bool Decoder::init_hw_decoder(AVCodecContext* ctx, const enum AVHWDeviceType type) {
    int err = 0;
    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0)) < 0) {
        std::cerr << "Failed to create specified HW device.\n";
        return false;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    return true;
}

bool Decoder::open(const std::string& path) {
    cleanup();

    frame = av_frame_alloc();
    sw_frame = av_frame_alloc();
    pkt = av_packet_alloc();

    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input: " << path << "\n";
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        return false;
    }

    const AVCodec* codec = nullptr;
    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_stream_idx < 0) {
        return false;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) return false;

    avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[video_stream_idx]->codecpar);

    // Multithreaded CPU Fallback Setup
    codec_ctx->thread_count = 0; // Auto
    codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    // Hardware Acceleration Setup
    // Fallbacks in order: D3D11VA -> DXVA2
    hw_active = false;
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;

        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
            if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA || config->device_type == AV_HWDEVICE_TYPE_DXVA2) {
                hw_pix_fmt = config->pix_fmt;
                codec_ctx->opaque = (void*)(intptr_t)hw_pix_fmt;
                codec_ctx->get_format = get_hw_format;

                if (init_hw_decoder(codec_ctx, config->device_type)) {
                    hw_active = true;
                    requested_hw_type = config->device_type;
                    std::cout << "Hardware acceleration initialized successfully.\n";
                    break;
                }
            }
        }
    }

    if (!hw_active) {
        std::cout << "Hardware acceleration unavailable or rejected. Using multi-threaded CPU fallback.\n";
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        return false;
    }

    return true;
}

bool Decoder::seek_to_frame(int64_t target_frame, double fps) {
    if (!fmt_ctx || video_stream_idx < 0) return false;

    // Exact seeking in frames using PTS
    AVStream* stream = fmt_ctx->streams[video_stream_idx];
    double time_in_seconds = (double)target_frame / fps;
    int64_t target_pts = (int64_t)(time_in_seconds / av_q2d(stream->time_base));

    // Seek to the keyframe prior to the target
    if (av_seek_frame(fmt_ctx, video_stream_idx, target_pts, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    avcodec_flush_buffers(codec_ctx);

    // We would step-decode here to reach the exact frame if necessary.
    // For scaffolding, this achieves the baseline seek.
    return true;
}

bool Decoder::read_next_frame(VideoFrame* out) {
    if (!fmt_ctx || !codec_ctx || !out) return false;

    while (true) {
        // Try to receive a frame first
        int ret = avcodec_receive_frame(codec_ctx, frame);

        if (ret == 0) {
            // Successfully received a decoded frame
            AVFrame* process_frame = frame;

            // Handle HW to SW Transfer
            if (frame->format == hw_pix_fmt) {
                av_frame_unref(sw_frame); // Unref before transfer
                if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                    av_frame_unref(frame);
                    return false;
                }
                process_frame = sw_frame;
            }

            // Convert format to common RGBA software buffer using swscale
            if (!sws_ctx) {
                sws_ctx = sws_getContext(
                    process_frame->width, process_frame->height, (AVPixelFormat)process_frame->format,
                    codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGBA,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                );
            }

            out->width = codec_ctx->width;
            out->height = codec_ctx->height;
            out->linesize = out->width * 4;
            out->pts = frame->pts;
            out->data.resize(out->height * out->linesize);

            uint8_t* dest_data[4] = { out->data.data(), nullptr, nullptr, nullptr };
            int dest_linesize[4] = { out->linesize, 0, 0, 0 };

            sws_scale(sws_ctx, process_frame->data, process_frame->linesize, 0,
                      process_frame->height, dest_data, dest_linesize);

            av_frame_unref(frame);
            if (process_frame == sw_frame) {
                av_frame_unref(sw_frame);
            }
            return true;
        } else if (ret == AVERROR_EOF) {
            // End of stream
            return false;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
            // Fatal decoding error
            return false;
        }

        // We need more data (ret == AVERROR(EAGAIN)), so read the next packet
        if (av_read_frame(fmt_ctx, pkt) < 0) {
            // EOF reached on the input container. Send flush packet.
            avcodec_send_packet(codec_ctx, nullptr);
            continue;
        }

        if (pkt->stream_index == video_stream_idx) {
            ret = avcodec_send_packet(codec_ctx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                av_packet_unref(pkt);
                return false;
            }
        }
        av_packet_unref(pkt);
    }
}
