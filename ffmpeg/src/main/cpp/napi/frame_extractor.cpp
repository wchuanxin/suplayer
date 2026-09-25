#include "frame_extractor.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <cstdint>
#include <string>
#include <vector>

namespace ffmpeg {
namespace {

struct FrameContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string src;
    int64_t timeMs = 0;
    std::string error;
    int32_t width = 0;
    int32_t height = 0;
    uint8_t* rgba = nullptr;
    size_t rgbaSize = 0;
};

std::string ErrToStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

void ExtractExecute(napi_env env, void* data)
{
    auto* ctx = static_cast<FrameContext*>(data);
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;

    int ret = avformat_open_input(&fmt, ctx->src.c_str(), nullptr, nullptr);
    if (ret < 0) {
        ctx->error = "avformat_open_input failed: " + ErrToStr(ret);
        goto cleanup;
    }
    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        ctx->error = "avformat_find_stream_info failed: " + ErrToStr(ret);
        goto cleanup;
    }

    {
        int streamIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            ctx->error = "no video stream found";
            goto cleanup;
        }
        AVStream* stream = fmt->streams[streamIndex];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            ctx->error = "unsupported video codec";
            goto cleanup;
        }
        dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, stream->codecpar);
        ret = avcodec_open2(dec, codec, nullptr);
        if (ret < 0) {
            ctx->error = "avcodec_open2 failed: " + ErrToStr(ret);
            goto cleanup;
        }

        if (ctx->timeMs > 0) {
            int64_t target = av_rescale_q(ctx->timeMs, AVRational{ 1, 1000 }, stream->time_base);
            ret = avformat_seek_file(fmt, streamIndex, INT64_MIN, target, INT64_MAX, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                // seeking unsupported: decode from start
                avcodec_flush_buffers(dec);
            }
        }

        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        bool gotFrame = false;
        while (!gotFrame) {
            ret = avcodec_receive_frame(dec, frame);
            if (ret == AVERROR(EAGAIN)) {
                ret = av_read_frame(fmt, pkt);
                if (ret < 0) {
                    ctx->error = "no frame at requested position";
                    goto cleanup;
                }
                if (pkt->stream_index == streamIndex) {
                    avcodec_send_packet(dec, pkt);
                }
                av_packet_unref(pkt);
                continue;
            }
            if (ret < 0) {
                ctx->error = "avcodec_receive_frame failed: " + ErrToStr(ret);
                goto cleanup;
            }
            gotFrame = true;
        }

        ctx->width = frame->width;
        ctx->height = frame->height;
        ctx->rgbaSize = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4;
        ctx->rgba = static_cast<uint8_t*>(malloc(ctx->rgbaSize));
        if (ctx->rgba == nullptr) {
            ctx->error = "out of memory";
            goto cleanup;
        }

        {
            uint8_t* dst[4] = { ctx->rgba, nullptr, nullptr, nullptr };
            int dstStride[4] = { frame->width * 4, 0, 0, 0 };
            sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws == nullptr) {
                ctx->error = "sws_getContext failed";
                goto cleanup;
            }
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        }
    }

cleanup:
    if (sws != nullptr) {
        sws_freeContext(sws);
    }
    if (pkt != nullptr) {
        av_packet_free(&pkt);
    }
    if (frame != nullptr) {
        av_frame_free(&frame);
    }
    if (dec != nullptr) {
        avcodec_free_context(&dec);
    }
    if (fmt != nullptr) {
        avformat_close_input(&fmt);
    }
}

void ExtractComplete(napi_env env, napi_status status, void* data)
{
    auto* ctx = static_cast<FrameContext*>(data);

    if (!ctx->error.empty()) {
        napi_value code = nullptr;
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, "ffmpeg:extract_error", NAPI_AUTO_LENGTH, &code);
        napi_create_string_utf8(env, ctx->error.c_str(), NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, code, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
    } else {
        void* abData = nullptr;
        napi_value ab = nullptr;
        napi_create_arraybuffer(env, ctx->rgbaSize, &abData, &ab);
        memcpy(abData, ctx->rgba, ctx->rgbaSize);

        napi_value result = nullptr;
        napi_create_object(env, &result);
        napi_value w = nullptr;
        napi_value h = nullptr;
        napi_create_int32(env, ctx->width, &w);
        napi_create_int32(env, ctx->height, &h);
        napi_set_named_property(env, result, "width", w);
        napi_set_named_property(env, result, "height", h);
        napi_set_named_property(env, result, "data", ab);
        napi_resolve_deferred(env, ctx->deferred, result);
    }

    if (ctx->rgba != nullptr) {
        free(ctx->rgba);
    }
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

} // namespace

napi_value ExtractFrame(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_type_error(env, nullptr, "extractFrame(src: string, timeMs: number) requires 2 arguments");
        return nullptr;
    }

    size_t len = 0;
    if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) {
        napi_throw_type_error(env, nullptr, "src must be a string");
        return nullptr;
    }
    std::vector<char> buf(len + 1, '\0');
    napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &len);

    double timeMs = 0.0;
    napi_get_value_double(env, args[1], &timeMs);

    auto* ctx = new FrameContext();
    ctx->src.assign(buf.data(), len);
    ctx->timeMs = static_cast<int64_t>(timeMs);

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_extract_frame", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExtractExecute, ExtractComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);

    return promise;
}

} // namespace ffmpeg
