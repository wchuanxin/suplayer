#include "video_decoder.h"
#include "net_options.h"
#include "hw_decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <native_window/external_window.h>
#include <native_buffer/buffer_common.h>
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <unistd.h>
}

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ffmpeg {
namespace {

enum MsgType {
    MSG_FRAME = 0,
    MSG_END = 1,
    MSG_ERROR = 2,
    MSG_INFO = 3,
};

struct DecoderMsg {
    int type = MSG_FRAME;
    double ptsMs = 0.0;
    std::string err;
    std::string backend;
};

std::string ErrToStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class VideoDecoderCore {
public:
    VideoDecoderCore() = default;
    ~VideoDecoderCore()
    {
        StopInternal();
        if (hw_ != nullptr) {
            hw_->Stop();
        }
        ReleaseWindow();
        CloseCodec();
    }

    napi_ref* FrameSlot()
    {
        return &frameRef_;
    }
    napi_ref* EndSlot()
    {
        return &endRef_;
    }
    napi_ref* ErrorSlot()
    {
        return &errorRef_;
    }
    napi_ref* InfoSlot()
    {
        return &infoRef_;
    }
    napi_ref GetFrameCallback() const
    {
        return frameRef_;
    }
    napi_ref GetEndCallback() const
    {
        return endRef_;
    }
    napi_ref GetErrorCallback() const
    {
        return errorRef_;
    }
    napi_ref GetInfoCallback() const
    {
        return infoRef_;
    }

    napi_threadsafe_function GetTsfn() const
    {
        return tsfn_;
    }
    void SetTsfn(napi_threadsafe_function tsfn)
    {
        tsfn_ = tsfn;
    }
    void ReleaseTsfn()
    {
        if (tsfn_ != nullptr) {
            napi_release_threadsafe_function(tsfn_, napi_tsfn_release);
            tsfn_ = nullptr;
        }
    }

    // Interrupt callback for the demuxer: aborts on stop_ or when a peek
    // deadline is armed (protects the packet-peek in TryStartHw from blocking
    // the JS thread on a stalled network).
    static int InterruptGuard(void* opaque)
    {
        auto* core = static_cast<VideoDecoderCore*>(opaque);
        if (core->stop_.load(std::memory_order_relaxed)) {
            return 1;
        }
        int64_t deadline = core->peekDeadlineMs_.load(std::memory_order_relaxed);
        if (deadline > 0 && NowMs() > deadline) {
            return 1;
        }
        return 0;
    }

    bool Open(const std::string& src, int streamIndex, const NetOptions& netOpts)
    {
        CloseCodec();
        hwMode_ = netOpts.hwMode;
        stop_ = false;
        fmt_ = avformat_alloc_context();
        fmt_->interrupt_callback.callback = &VideoDecoderCore::InterruptGuard;
        fmt_->interrupt_callback.opaque = this;
        AVDictionary* dict = nullptr;
        ApplyOpenOptions(fmt_, &dict, netOpts);
        int ret = avformat_open_input(&fmt_, src.c_str(), nullptr, &dict);
        av_dict_free(&dict);
        if (ret < 0) {
            lastError_ = "avformat_open_input failed: " + ErrToStr(ret);
            return false;
        }
        ret = avformat_find_stream_info(fmt_, nullptr);
        if (ret < 0) {
            lastError_ = "avformat_find_stream_info failed: " + ErrToStr(ret);
            CloseCodec();
            return false;
        }
        AVProgram* program = nullptr;
        if (netOpts.program != INT32_MIN) {
            program = SelectProgram(fmt_, netOpts.program);
            if (program == nullptr) {
                lastError_ = "program not found: " + std::to_string(netOpts.program);
                CloseCodec();
                return false;
            }
        }
        if (streamIndex < 0) {
            if (program != nullptr) {
                streamIndex = FindStreamInProgram(fmt_, program, AVMEDIA_TYPE_VIDEO);
            } else {
                streamIndex = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            }
            if (streamIndex < 0) {
                lastError_ = "no video stream found";
                CloseCodec();
                return false;
            }
        }
        if (static_cast<unsigned int>(streamIndex) >= fmt_->nb_streams) {
            lastError_ = "stream index out of range";
            CloseCodec();
            return false;
        }
        if (!StreamInProgram(program, streamIndex)) {
            lastError_ = "stream index " + std::to_string(streamIndex) + " does not belong to program " +
                std::to_string(netOpts.program);
            CloseCodec();
            return false;
        }
        streamIndex_ = streamIndex;
        AVStream* stream = fmt_->streams[streamIndex_];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            lastError_ = "unsupported video codec";
            CloseCodec();
            return false;
        }
        dec_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec_, stream->codecpar);
        ret = avcodec_open2(dec_, codec, nullptr);
        if (ret < 0) {
            lastError_ = "avcodec_open2 failed: " + ErrToStr(ret);
            CloseCodec();
            return false;
        }
        width_ = dec_->width;
        height_ = dec_->height;
        AVRational fr = stream->avg_frame_rate;
        fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        opened_ = true;
        return true;
    }

    bool IsOpened() const
    {
        return opened_;
    }
    const std::string& GetLastError() const
    {
        return lastError_;
    }

    int GetWidth() const
    {
        return width_;
    }
    int GetHeight() const
    {
        return height_;
    }
    double GetFps() const
    {
        return fps_;
    }
    int64_t GetDurationMs() const
    {
        if (!opened_ || fmt_ == nullptr) {
            return 0;
        }
        return fmt_->duration > 0 ? fmt_->duration / (AV_TIME_BASE / 1000) : 0;
    }
    std::string GetCodecName() const
    {
        if (dec_ == nullptr || dec_->codec == nullptr || dec_->codec->name == nullptr) {
            return "";
        }
        return dec_->codec->name;
    }

    bool Start(uint64_t surfaceId)
    {
        if (!opened_ || window_ != nullptr || running_.load()) {
            return false;
        }
        stop_ = false;
        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window_) != 0 || window_ == nullptr) {
            lastError_ = "create native window from surfaceId failed";
            OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideo", "create window failed surfaceId=%{public}llu",
                static_cast<unsigned long long>(surfaceId));
            ReleaseWindow();
            return false;
        }
        if (!TryStartHw()) {
            if (hwMode_ == HwMode::FORCE) {
                hw_.reset();
                ReleaseWindow();
                return false;
            }
            hw_.reset();
        }
        if (!hwActive_) {
            // software path owns the window buffer format
            int32_t g1 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, width_, height_);
            int32_t g2 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
            uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
            int32_t g3 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_USAGE, usage);
            (void)g1;
            (void)g2;
            (void)g3;
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo", "window created surfaceId=%{public}llu %{public}dx%{public}d backend=%{public}s",
            static_cast<unsigned long long>(surfaceId), width_, height_, hwActive_.load() ? "hw" : "sw");
        EmitInfo(hwActive_.load() ? "hw" : "sw");
        paused_ = false;
        if (running_.exchange(true)) {
            return true;
        }
        worker_ = std::thread(&VideoDecoderCore::Run, this);
        return true;
    }

    void Pause()
    {
        paused_ = true;
        if (hw_ != nullptr) {
            hw_->Pause();
        }
    }
    void Resume()
    {
        paused_ = false;
        if (hw_ != nullptr) {
            hw_->Resume();
        }
    }
    void Stop()
    {
        StopInternal();
        if (hw_ != nullptr) {
            hw_->Stop();
        }
        ReleaseWindow();
    }

private:
    void StopInternal()
    {
        stop_ = true;
        if (!running_.exchange(false)) {
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void ReleaseWindow()
    {
        if (window_ != nullptr) {
            OH_NativeWindow_DestroyNativeWindow(window_);
            window_ = nullptr;
        }
    }

    void CloseCodec()
    {
        if (hwPendingPkt_ != nullptr) {
            av_packet_free(&hwPendingPkt_);
        }
        if (sws_ != nullptr) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (dec_ != nullptr) {
            avcodec_free_context(&dec_);
        }
        if (fmt_ != nullptr) {
            avformat_close_input(&fmt_);
        }
        opened_ = false;
    }

    void Emit(DecoderMsg* msg)
    {
        if (tsfn_ == nullptr) {
            delete msg;
            return;
        }
        napi_call_threadsafe_function(tsfn_, msg, napi_tsfn_blocking);
    }

    void EmitFrame(double ptsMs)
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_FRAME;
        msg->ptsMs = ptsMs;
        Emit(msg);
    }

    void EmitEnd()
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_END;
        Emit(msg);
    }

    void EmitError(const std::string& err)
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_ERROR;
        msg->err = err;
        Emit(msg);
    }

    void EmitInfo(const std::string& backend)
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_INFO;
        msg->backend = backend;
        Emit(msg);
    }

    // Decide whether the current stream is eligible for the hardware decoder and
    // try to bring it up. On success hwActive_ is set. Eligibility: h264/hevc with
    // Annex-B packet framing (MPEG-TS / HLS-TS). Note that TS demuxers may still
    // synthesize hvcC/avcC extradata, so extradata presence is NOT a reliable
    // signal; instead the first video packet is peeked and checked for an Annex-B
    // start code. The peek is served from the demuxer's probe buffer (no network
    // I/O) and is protected by a short interrupt deadline anyway. The peeked
    // packet is kept as hwPendingPkt_ so neither the hw nor the sw path loses the
    // first (key)frame.
    bool TryStartHw()
    {
        if (hwMode_ == HwMode::NEVER) {
            return false;
        }
        if (dec_ == nullptr || fmt_ == nullptr) {
            lastError_ = "decoder is not opened";
            return false;
        }
        AVCodecID codecId = dec_->codec_id;
        if (codecId != AV_CODEC_ID_H264 && codecId != AV_CODEC_ID_HEVC) {
            lastError_ = "hardware decode is not supported for codec id=" + std::to_string(static_cast<int>(codecId));
            return false;
        }
        AVPacket* peek = av_packet_alloc();
        bool annexb = false;
        peekDeadlineMs_.store(NowMs() + 3000);
        while (!stop_.load(std::memory_order_relaxed)) {
            int ret = av_read_frame(fmt_, peek);
            if (ret < 0) {
                lastError_ = "peek first video packet failed: " + ErrToStr(ret);
                break;
            }
            if (peek->stream_index == streamIndex_) {
                if (peek->size >= 4) {
                    const uint8_t* d = peek->data;
                    annexb = (d[0] == 0x00 && d[1] == 0x00 &&
                        (d[2] == 0x01 || (d[2] == 0x00 && d[3] == 0x01)));
                }
                if (annexb) {
                    hwPendingPkt_ = peek;
                    peek = nullptr;
                } else {
                    lastError_ = "stream is not Annex-B (length-prefixed packets, e.g. MP4)";
                }
                break;
            }
            av_packet_unref(peek);
        }
        peekDeadlineMs_.store(0);
        if (peek != nullptr) {
            av_packet_free(&peek);
        }
        if (!annexb) {
            if (hwPendingPkt_ != nullptr) {
                av_packet_free(&hwPendingPkt_);
            }
            return false;
        }
        const char* mime = (codecId == AV_CODEC_ID_HEVC) ? OH_AVCODEC_MIMETYPE_VIDEO_HEVC
                                                         : OH_AVCODEC_MIMETYPE_VIDEO_AVC;
        hw_ = std::make_unique<HwVideoDecoder>();
        hw_->onFrame = [this](double ptsMs) { EmitFrame(ptsMs); };
        hw_->onEnd = [this]() { EmitEnd(); };
        hw_->onError = [this](const std::string& err) { EmitError(err); };
        if (!hw_->Start(window_, mime, width_, height_, fps_)) {
            lastError_ = hw_->GetLastError();
            if (hwMode_ == HwMode::AUTO) {
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                    "hw decoder start failed, fallback to sw: %{public}s", lastError_.c_str());
            }
            return false;
        }
        hwActive_ = true;
        return true;
    }

    bool RenderFrame(const AVFrame* frame)
    {
        OHNativeWindowBuffer* buffer = nullptr;
        int fenceFd = -1;
        int32_t ret = OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fenceFd);
        if (ret != 0 || buffer == nullptr) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "request buffer failed ret=%{public}d", ret);
            return false;
        }

        OH_NativeBuffer* nb = nullptr;
        if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &nb) != 0 || nb == nullptr) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "from native window buffer failed");
            return false;
        }

        void* virAddr = nullptr;
        int32_t mapRet = 0;
        bool mapped = false;
        if (fenceFd >= 0) {
            mapRet = OH_NativeBuffer_MapWaitFence(nb, fenceFd, &virAddr);
        } else {
            mapRet = OH_NativeBuffer_Map(nb, &virAddr);
        }
        if (mapRet == 0 && virAddr != nullptr) {
            mapped = true;
        } else {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            // fallback: some devices pre-map the buffer handle
            BufferHandle* fb = OH_NativeWindow_GetBufferHandleFromNative(buffer);
            if (fb != nullptr && fb->virAddr != nullptr) {
                virAddr = fb->virAddr;
            } else {
                OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "map failed ret=%{public}d vir=%{public}d",
                    mapRet, virAddr != nullptr);
                return false;
            }
        }

        BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
        if (handle == nullptr) {
            OH_NativeBuffer_Unmap(nb);
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
            return false;
        }

        if (sws_ == nullptr || swsW_ != frame->width || swsH_ != frame->height ||
            swsFmt_ != static_cast<AVPixelFormat>(frame->format)) {
            if (sws_ != nullptr) {
                sws_freeContext(sws_);
            }
            swsW_ = frame->width;
            swsH_ = frame->height;
            swsFmt_ = static_cast<AVPixelFormat>(frame->format);
            sws_ = sws_getContext(swsW_, swsH_, swsFmt_,
                swsW_, swsH_, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws_ == nullptr) {
                OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
                return false;
            }
        }

        uint8_t* dst[4] = { static_cast<uint8_t*>(virAddr), nullptr, nullptr, nullptr };
        int dstStride[4] = { handle->stride, 0, 0, 0 };
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);

        if (mapped) {
            OH_NativeBuffer_Unmap(nb);
        }

        Region region = { nullptr, 0 };
        ret = OH_NativeWindow_NativeWindowFlushBuffer(window_, buffer, -1, region);
        if (ret != 0) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "flush failed ret=%{public}d", ret);
            return false;
        }
        return true;
    }

    // Hardware path: demux and push Annex-B packets into HwVideoDecoder; frame/end
    // events arrive from its render thread via the shared tsfn channel.
    void RunHw()
    {
        AVPacket* pkt = av_packet_alloc();
        AVRational usTimeBase = { 1, 1000000 };
        if (hwPendingPkt_ != nullptr) {
            AVStream* stream = fmt_->streams[streamIndex_];
            int64_t ptsUs = 0;
            if (hwPendingPkt_->pts != AV_NOPTS_VALUE) {
                ptsUs = av_rescale_q(hwPendingPkt_->pts, stream->time_base, usTimeBase);
            } else if (hwPendingPkt_->dts != AV_NOPTS_VALUE) {
                ptsUs = av_rescale_q(hwPendingPkt_->dts, stream->time_base, usTimeBase);
            }
            hw_->FeedPacket(hwPendingPkt_->data, hwPendingPkt_->size, ptsUs, false);
            av_packet_free(&hwPendingPkt_);
        }
        while (!stop_) {
            if (paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int ret = av_read_frame(fmt_, pkt);
            if (ret < 0) {
                hw_->FeedPacket(nullptr, 0, 0, true);
                break;
            }
            if (pkt->stream_index == streamIndex_) {
                AVStream* stream = fmt_->streams[streamIndex_];
                int64_t ptsUs = 0;
                if (pkt->pts != AV_NOPTS_VALUE) {
                    ptsUs = av_rescale_q(pkt->pts, stream->time_base, usTimeBase);
                } else if (pkt->dts != AV_NOPTS_VALUE) {
                    ptsUs = av_rescale_q(pkt->dts, stream->time_base, usTimeBase);
                }
                if (!hw_->FeedPacket(pkt->data, pkt->size, ptsUs, false)) {
                    break;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    void Run()
    {
        if (hwActive_.load()) {
            RunHw();
            return;
        }
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool eofSent = false;
        int64_t firstPts = AV_NOPTS_VALUE;
        auto startWall = std::chrono::steady_clock::now();
        int rendered = 0;
        int failed = 0;
        if (hwPendingPkt_ != nullptr) {
            // packet consumed by the hw eligibility peek when falling back to sw
            avcodec_send_packet(dec_, hwPendingPkt_);
            av_packet_free(&hwPendingPkt_);
        }

        while (!stop_) {
            if (paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int ret = avcodec_receive_frame(dec_, frame);
            if (ret == AVERROR(EAGAIN)) {
                if (eofSent) {
                    EmitEnd();
                    break;
                }
                ret = av_read_frame(fmt_, pkt);
                if (ret < 0) {
                    avcodec_send_packet(dec_, nullptr);
                    eofSent = true;
                    continue;
                }
                if (pkt->stream_index == streamIndex_) {
                    avcodec_send_packet(dec_, pkt);
                }
                av_packet_unref(pkt);
                continue;
            }
            if (ret == AVERROR_EOF) {
                EmitEnd();
                break;
            }
            if (ret < 0) {
                EmitError("avcodec_receive_frame failed: " + ErrToStr(ret));
                break;
            }

            AVStream* stream = fmt_->streams[streamIndex_];
            int64_t pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                pts = frame->pts;
            }
            double ptsMs = 0.0;
            if (pts != AV_NOPTS_VALUE) {
                ptsMs = static_cast<double>(pts) * av_q2d(stream->time_base) * 1000.0;
            }

            // real-time pacing based on pts delta
            if (firstPts == AV_NOPTS_VALUE) {
                firstPts = pts;
                startWall = std::chrono::steady_clock::now();
            } else if (pts != AV_NOPTS_VALUE) {
                double targetMs = static_cast<double>(pts - firstPts) * av_q2d(stream->time_base) * 1000.0;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startWall).count();
                int64_t waitMs = static_cast<int64_t>(targetMs) - static_cast<int64_t>(elapsed);
                if (waitMs > 0 && waitMs < 1000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                }
            }

            RenderFrame(frame) ? rendered++ : failed++;
            EmitFrame(ptsMs);
            av_frame_unref(frame);
        }

        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo", "render done rendered=%{public}d failed=%{public}d",
            rendered, failed);

        av_frame_free(&frame);
        av_packet_free(&pkt);
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    SwsContext* sws_ = nullptr;
    OHNativeWindow* window_ = nullptr;
    std::unique_ptr<HwVideoDecoder> hw_;
    int streamIndex_ = -1;
    int width_ = 0;
    int height_ = 0;
    int swsW_ = 0;
    int swsH_ = 0;
    AVPixelFormat swsFmt_ = AV_PIX_FMT_NONE;
    double fps_ = 0.0;
    HwMode hwMode_ = HwMode::AUTO;
    std::atomic<bool> hwActive_{ false };
    AVPacket* hwPendingPkt_ = nullptr;
    std::atomic<int64_t> peekDeadlineMs_{ 0 };
    bool opened_ = false;
    std::string lastError_;

    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> paused_{ false };

    napi_threadsafe_function tsfn_ = nullptr;
    napi_ref frameRef_ = nullptr;
    napi_ref endRef_ = nullptr;
    napi_ref errorRef_ = nullptr;
    napi_ref infoRef_ = nullptr;
};

struct OpenContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    VideoDecoderCore* core = nullptr;
    std::string src;
    int streamIndex = -1;
    NetOptions netOpts;
    bool ok = false;
};

VideoDecoderCore* GetCore(napi_env env, napi_callback_info info)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVar, nullptr);
    VideoDecoderCore* core = nullptr;
    napi_unwrap(env, thisVar, reinterpret_cast<void**>(&core));
    return core;
}

void OpenExecute(napi_env env, void* data)
{
    auto* ctx = static_cast<OpenContext*>(data);
    ctx->ok = ctx->core->Open(ctx->src, ctx->streamIndex, ctx->netOpts);
}

void OpenComplete(napi_env env, napi_status status, void* data)
{
    auto* ctx = static_cast<OpenContext*>(data);
    if (!ctx->ok) {
        napi_value code = nullptr;
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, "ffmpeg:open_error", NAPI_AUTO_LENGTH, &code);
        napi_create_string_utf8(env, ctx->core->GetLastError().c_str(), NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, code, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
    } else {
        napi_value result = nullptr;
        napi_create_object(env, &result);
        napi_value v = nullptr;
        napi_create_int32(env, ctx->core->GetWidth(), &v);
        napi_set_named_property(env, result, "width", v);
        napi_create_int32(env, ctx->core->GetHeight(), &v);
        napi_set_named_property(env, result, "height", v);
        napi_create_double(env, ctx->core->GetFps(), &v);
        napi_set_named_property(env, result, "fps", v);
        napi_create_int64(env, ctx->core->GetDurationMs(), &v);
        napi_set_named_property(env, result, "durationMs", v);
        napi_create_string_utf8(env, ctx->core->GetCodecName().c_str(), NAPI_AUTO_LENGTH, &v);
        napi_set_named_property(env, result, "codec", v);
        napi_resolve_deferred(env, ctx->deferred, result);
    }
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

void TsfnDispatcher(napi_env env, napi_value jsCallback, void* context, void* data)
{
    (void)jsCallback;
    auto* core = static_cast<VideoDecoderCore*>(context);
    auto* msg = static_cast<DecoderMsg*>(data);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);

    if (msg->type == MSG_FRAME) {
        if (core != nullptr && core->GetFrameCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetFrameCallback(), &cb);
            if (cb != nullptr) {
                napi_value ev = nullptr;
                napi_create_object(env, &ev);
                napi_value pts = nullptr;
                napi_create_double(env, msg->ptsMs, &pts);
                napi_set_named_property(env, ev, "ptsMs", pts);
                napi_call_function(env, undefined, cb, 1, &ev, &undefined);
            }
        }
    } else if (msg->type == MSG_END) {
        if (core != nullptr && core->GetEndCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetEndCallback(), &cb);
            if (cb != nullptr) {
                napi_call_function(env, undefined, cb, 0, nullptr, &undefined);
            }
        }
    } else if (msg->type == MSG_INFO) {
        if (core != nullptr && core->GetInfoCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetInfoCallback(), &cb);
            if (cb != nullptr) {
                napi_value ev = nullptr;
                napi_create_object(env, &ev);
                napi_value backend = nullptr;
                napi_create_string_utf8(env, msg->backend.c_str(), NAPI_AUTO_LENGTH, &backend);
                napi_set_named_property(env, ev, "backend", backend);
                napi_call_function(env, undefined, cb, 1, &ev, &undefined);
            }
        }
    } else if (msg->type == MSG_ERROR) {
        if (core != nullptr && core->GetErrorCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetErrorCallback(), &cb);
            if (cb != nullptr) {
                napi_value msgVal = nullptr;
                napi_create_string_utf8(env, msg->err.c_str(), NAPI_AUTO_LENGTH, &msgVal);
                napi_call_function(env, undefined, cb, 1, &msgVal, &undefined);
            }
        }
    }
    delete msg;
}

void CoreFinalize(napi_env env, void* data, void* hint)
{
    (void)hint;
    auto* core = static_cast<VideoDecoderCore*>(data);
    core->ReleaseTsfn();
    napi_ref frame = core->GetFrameCallback();
    napi_ref end = core->GetEndCallback();
    napi_ref err = core->GetErrorCallback();
    napi_ref info = core->GetInfoCallback();
    if (frame != nullptr) {
        napi_delete_reference(env, frame);
    }
    if (end != nullptr) {
        napi_delete_reference(env, end);
    }
    if (err != nullptr) {
        napi_delete_reference(env, err);
    }
    if (info != nullptr) {
        napi_delete_reference(env, info);
    }
    delete core;
}

} // namespace

napi_value FFVideoDecoderConstructor(napi_env env, napi_callback_info info)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVar, nullptr);
    auto* core = new VideoDecoderCore();
    napi_wrap(env, thisVar, core, CoreFinalize, nullptr, nullptr);
    return thisVar;
}

napi_value FFVideoDecoderOpen(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = { nullptr, nullptr, nullptr };
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, &argc, args, &thisVar, nullptr);
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "open(src: string, streamIndex?: number, options?: object) requires src");
        return nullptr;
    }

    size_t len = 0;
    if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) {
        napi_throw_type_error(env, nullptr, "src must be a string");
        return nullptr;
    }
    std::vector<char> buf(len + 1, '\0');
    napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &len);

    int32_t streamIndex = -1;
    if (argc >= 2) {
        napi_get_value_int32(env, args[1], &streamIndex);
    }

    auto* ctx = new OpenContext();
    ctx->core = core;
    ctx->src.assign(buf.data(), len);
    ctx->streamIndex = streamIndex;
    if (argc >= 3 && !ParseNetOptions(env, args[2], ctx->netOpts)) {
        delete ctx;
        napi_throw_type_error(env, nullptr,
            "options must be { userAgent?, headers?, allowedExtensions?, liveStartIndex?, analyzeMs?, hw?, program? }");
        return nullptr;
    }

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_video_open", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, thisVar, resourceName, OpenExecute, OpenComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value FFVideoDecoderOn(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr || argc < 2) {
        napi_throw_type_error(env, nullptr, "on(event: string, callback) requires 2 arguments");
        return nullptr;
    }

    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    std::vector<char> ev(len + 1, '\0');
    napi_get_value_string_utf8(env, args[0], ev.data(), ev.size(), &len);
    std::string event(ev.data(), len);

    napi_ref* slot = nullptr;
    if (event == "frame") {
        slot = core->FrameSlot();
    } else if (event == "end") {
        slot = core->EndSlot();
    } else if (event == "error") {
        slot = core->ErrorSlot();
    } else if (event == "info") {
        slot = core->InfoSlot();
    } else {
        napi_throw_type_error(env, nullptr, "unsupported event, expect 'frame' | 'end' | 'error' | 'info'");
        return nullptr;
    }
    if (*slot != nullptr) {
        napi_delete_reference(env, *slot);
    }
    napi_create_reference(env, args[1], 1, slot);
    return nullptr;
}

napi_value FFVideoDecoderStart(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    if (!core->IsOpened()) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not opened");
        return nullptr;
    }
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "start(surfaceId: string) requires surfaceId");
        return nullptr;
    }

    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    std::vector<char> buf(len + 1, '\0');
    napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &len);
    std::string surfaceIdStr(buf.data(), len);
    uint64_t surfaceId = strtoull(surfaceIdStr.c_str(), nullptr, 10);
    if (surfaceId == 0) {
        napi_throw_type_error(env, nullptr, "invalid surfaceId");
        return nullptr;
    }

    if (core->GetTsfn() == nullptr) {
        napi_value resourceName = nullptr;
        napi_create_string_utf8(env, "ffmpeg_video_tsfn", NAPI_AUTO_LENGTH, &resourceName);
        napi_threadsafe_function tsfn = nullptr;
        napi_create_threadsafe_function(env, nullptr, nullptr, resourceName, 32, 2,
            nullptr, nullptr, core, TsfnDispatcher, &tsfn);
        core->SetTsfn(tsfn);
    }

    if (!core->Start(surfaceId)) {
        napi_throw_error(env, nullptr, core->GetLastError().c_str());
        return nullptr;
    }
    return nullptr;
}

napi_value FFVideoDecoderPause(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Pause();
    }
    return nullptr;
}

napi_value FFVideoDecoderResume(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Resume();
    }
    return nullptr;
}

napi_value FFVideoDecoderStop(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Stop();
    }
    return nullptr;
}

napi_value FFVideoDecoderClose(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Stop();
        core->ReleaseTsfn();
    }
    return nullptr;
}

} // namespace ffmpeg
