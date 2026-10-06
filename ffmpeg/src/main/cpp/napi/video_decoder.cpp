#include "video_decoder.h"
#include "net_options.h"
#include "hw_decoder.h"
#include "demux_session.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <native_window/external_window.h>
#include <native_buffer/buffer_common.h>
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <unistd.h>
}

#include <algorithm>
#include <atomic>
#include <cstring>
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

// Maps an FFmpeg AV_PROFILE_* value to the matching OHOS AVC_PROFILE_* /
// HEVC_PROFILE_* capability enum. Returns -1 (unknown) for anything else so
// the capability pre-check never rejects an unrecognized profile.
int MapOhProfile(AVCodecID codecId, int ffProfile)
{
    if (ffProfile == AV_PROFILE_UNKNOWN) {
        return -1;
    }
    switch (codecId) {
        case AV_CODEC_ID_H264:
            if (ffProfile == AV_PROFILE_H264_BASELINE) {
                return AVC_PROFILE_BASELINE;
            }
            if (ffProfile == AV_PROFILE_H264_MAIN) {
                return AVC_PROFILE_MAIN;
            }
            if (ffProfile == AV_PROFILE_H264_HIGH) {
                return AVC_PROFILE_HIGH;
            }
            return -1;
        case AV_CODEC_ID_HEVC:
            if (ffProfile == AV_PROFILE_HEVC_MAIN) {
                return HEVC_PROFILE_MAIN;
            }
            if (ffProfile == AV_PROFILE_HEVC_MAIN_10) {
                return HEVC_PROFILE_MAIN_10;
            }
            if (ffProfile == AV_PROFILE_HEVC_MAIN_STILL_PICTURE) {
                return HEVC_PROFILE_MAIN_STILL;
            }
            return -1;
        default:
            return -1;
    }
}

// Color metadata -> human-readable strings (names follow ffprobe conventions).
std::string ColorPrimariesToStr(int primaries)
{
    switch (primaries) {
        case AVCOL_PRI_BT709:
            return "bt709";
        case AVCOL_PRI_BT470M:
            return "bt470m";
        case AVCOL_PRI_BT470BG:
            return "bt470bg";
        case AVCOL_PRI_SMPTE170M:
            return "smpte170m";
        case AVCOL_PRI_SMPTE240M:
            return "smpte240m";
        case AVCOL_PRI_FILM:
            return "film";
        case AVCOL_PRI_BT2020:
            return "bt2020";
        case AVCOL_PRI_SMPTE431:
            return "smpte431";
        case AVCOL_PRI_SMPTE432:
            return "smpte432";
        default:
            return "";
    }
}

std::string ColorTrcToStr(int trc)
{
    switch (trc) {
        case AVCOL_TRC_BT709:
            return "bt709";
        case AVCOL_TRC_GAMMA22:
            return "gamma22";
        case AVCOL_TRC_GAMMA28:
            return "gamma28";
        case AVCOL_TRC_SMPTE170M:
            return "smpte170m";
        case AVCOL_TRC_SMPTE240M:
            return "smpte240m";
        case AVCOL_TRC_LINEAR:
            return "linear";
        case AVCOL_TRC_LOG:
            return "log100";
        case AVCOL_TRC_LOG_SQRT:
            return "log316";
        case AVCOL_TRC_IEC61966_2_4:
            return "iec61966-2-4";
        case AVCOL_TRC_BT2020_10:
            return "bt2020-10";
        case AVCOL_TRC_BT2020_12:
            return "bt2020-12";
        case AVCOL_TRC_SMPTE2084:
            return "smpte2084";
        case AVCOL_TRC_SMPTE428:
            return "smpte428";
        case AVCOL_TRC_ARIB_STD_B67:
            return "arib-std-b67";
        default:
            return "";
    }
}

std::string ColorRangeToStr(int range)
{
    switch (range) {
        case AVCOL_RANGE_MPEG:
            return "tv";
        case AVCOL_RANGE_JPEG:
            return "pc";
        default:
            return "";
    }
}

// Window color gamut for the render pipeline: BT.2020 sources are mapped to
// their HDR variant (PQ/HLG) so the system compositor tone-maps correctly.
// Returns -1 when the content is standard-dynamic-range (window default).
int MapWindowGamut(int primaries, int trc)
{
    if (primaries != AVCOL_PRI_BT2020) {
        return -1;
    }
    if (trc == AVCOL_TRC_SMPTE2084) {
        return NATIVEBUFFER_COLOR_GAMUT_BT2100_PQ;
    }
    if (trc == AVCOL_TRC_ARIB_STD_B67) {
        return NATIVEBUFFER_COLOR_GAMUT_BT2100_HLG;
    }
    return NATIVEBUFFER_COLOR_GAMUT_BT2020;
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
        frameEventMs_ = netOpts.frameEventMs;
        frameEmitted_ = false;
        stop_ = false;
        AVProgram* program = nullptr;
        if (netOpts.sharedDemux) {
            // 共享 demux 会话：一条连接服务同源的视频/音频/字幕解码器。
            // open/流分析/program 锁定/startTimeMs seek 均由会话统一完成，
            // 本解码器仅附加订阅并保留自己的目标前丢帧过滤。
            std::string err;
            session_ = DemuxSession::GetOrCreate(src, netOpts, err);
            if (session_ == nullptr) {
                lastError_ = "shared demux open failed: " + err;
                return false;
            }
            fmt_ = session_->fmt(); // borrowed: closed by the session
            sharedFmt_ = true;
            program = session_->program();
            dropBeforeMs_ = netOpts.startTimeMs > 0 ? static_cast<double>(netOpts.startTimeMs) : 0.0;
        } else {
            sharedFmt_ = false;
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
            if (netOpts.program != INT32_MIN) {
                program = SelectProgram(fmt_, netOpts.program);
                if (program == nullptr) {
                    lastError_ = "program not found: " + std::to_string(netOpts.program);
                    CloseCodec();
                    return false;
                }
            }
            // startTimeMs 定位起播（续播/seek）：跳到目标前最近关键帧，解码后的
            // 目标前帧在渲染循环丢弃，实现精确起点；不可 seek（直播等）时回退从头播放
            dropBeforeMs_ = 0.0;
            if (netOpts.startTimeMs > 0) {
                int64_t targetUs = static_cast<int64_t>(netOpts.startTimeMs) * 1000;
                if (avformat_seek_file(fmt_, -1, INT64_MIN, targetUs, INT64_MAX, 0) >= 0) {
                    dropBeforeMs_ = static_cast<double>(netOpts.startTimeMs);
                } else {
                    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                        "seek to %{public}lldms failed, starting from head",
                        static_cast<long long>(netOpts.startTimeMs));
                }
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
        // startTimeMs 的流级 seek 已在上方私有路径 / 会话创建时完成
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
        int openRet = avcodec_open2(dec_, codec, nullptr);
        if (openRet < 0) {
            lastError_ = "avcodec_open2 failed: " + ErrToStr(openRet);
            CloseCodec();
            return false;
        }
        width_ = dec_->width;
        height_ = dec_->height;
        AVRational fr = stream->avg_frame_rate;
        fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        if (sharedFmt_ && session_ != nullptr) {
            queue_ = session_->Subscribe({ streamIndex_ });
            // Live video: catch-up overflow policy. A slow (e.g. software
            // decoded 1080p50) video consumer must never block the shared
            // demux thread — that would starve the audio subscribers.
            queue_->SetLiveDrop(netOpts.live, true);
        }
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
    std::string GetColorPrimaries() const
    {
        return (opened_ && dec_ != nullptr) ? ColorPrimariesToStr(dec_->color_primaries) : "";
    }
    std::string GetColorTransfer() const
    {
        return (opened_ && dec_ != nullptr) ? ColorTrcToStr(dec_->color_trc) : "";
    }
    std::string GetColorRange() const
    {
        return (opened_ && dec_ != nullptr) ? ColorRangeToStr(dec_->color_range) : "";
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
            if (g1 != 0 || g2 != 0 || g3 != 0) {
                // Geometry is asynchronously negotiated with the consumer; even a
                // success here does not guarantee the first requested buffers match
                // width_/height_. RenderFrame validates every buffer and falls back
                // to a clamped copy, so a pending/failed geometry change can no
                // longer cause out-of-bounds writes (was: heap corruption crash).
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                    "window setup: geometry=%{public}d format=%{public}d usage=%{public}d", g1, g2, g3);
            }
        } else if (dec_ != nullptr) {
            // HDR/WCG hardware decode: the codec service renders NV12 straight
            // into the window; declare the content color gamut so the system
            // compositor tone-maps HDR10/HLG correctly.
            int gamut = MapWindowGamut(dec_->color_primaries, dec_->color_trc);
            if (gamut >= 0) {
                int32_t gr = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_COLOR_GAMUT, gamut);
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                    "window color gamut=%{public}d ret=%{public}d (primaries=%{public}d trc=%{public}d)",
                    gamut, gr, dec_->color_primaries, dec_->color_trc);
            }
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

    // A/V resync: drop the shared-queue backlog and resume at the newest
    // queued keyframe (≈ live edge). The still-running worker keeps decoding
    // through the gap: the next fed packet is an IDR, so the codec restarts
    // cleanly, and the pacing loop re-anchors on the forward pts jump
    // (waitMs >= 1000), exactly like a native live-overflow skip.
    void FlushToKeyframe()
    {
        if (queue_ == nullptr) {
            return; // private connection: no dropable backlog
        }
        queue_->DropBacklogToKeyframe();
    }

    // Leak-path zombie cleanup: unsubscribe from the shared demux session
    // WITHOUT joining anything (must stay safe on the JS thread while the
    // worker still runs). The worker's Pop fails fast once the queue is
    // detached, it feeds EOS and exits on its own. The session reference is
    // released on a helper thread so a possible last-reference Shutdown
    // (interrupt + join demux thread + close connection) never runs on the
    // JS thread; RunHw/Run hold their own session reference, keeping fmt_
    // valid until the worker is done.
    void DetachDemux()
    {
        if (session_ == nullptr) {
            return;
        }
        if (queue_ != nullptr) {
            session_->DetachQueue(queue_.get());
        }
        session_->CancelClaim();
        auto session = std::move(session_);
        session_ = nullptr;
        std::thread([session = std::move(session)]() mutable { session.reset(); }).detach();
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo", "demux detached (leak-path cleanup)");
    }

private:
    void StopInternal()
    {
        stop_ = true;
        // Wake a worker blocked in a shared-queue Pop so the join below
        // cannot hang on an empty, stalled queue (abort is re-checked on wake).
        if (queue_ != nullptr) {
            queue_->Wake();
        }
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
        if (rgbaScratch_ != nullptr) {
            av_free(rgbaScratch_);
            rgbaScratch_ = nullptr;
            rgbaScratchCap_ = 0;
        }
        swGeoLogged_ = false;
        if (dec_ != nullptr) {
            avcodec_free_context(&dec_);
        }
        // Shared demux: detach the per-subscriber queue first (drops the
        // session's routing reference and wakes the demux thread if it is
        // blocked on this queue's high-water mark), then drop the session
        // reference. The last reference closes the connection.
        if (queue_ != nullptr && session_ != nullptr) {
            session_->DetachQueue(queue_.get());
        }
        queue_ = nullptr;
        if (session_ != nullptr) {
            // Return a claim that Open reserved but Subscribe never consumed
            // (mid-open failure); no-op when the claim was already used.
            session_->CancelClaim();
            session_ = nullptr;
        }
        if (fmt_ != nullptr && !sharedFmt_) {
            avformat_close_input(&fmt_);
        }
        fmt_ = nullptr;
        sharedFmt_ = false;
        opened_ = false;
    }

    // Non-blocking TSF emit. A full queue means the JS thread is not draining
    // (busy, or blocked in stop()/close() joining this very worker / the hw
    // render thread). Blocking here would deadlock that join (6s thread-block
    // kill). Realtime frame/info events drop on a full queue; end/error retry
    // briefly and give up as soon as stop_ is armed, so StopInternal's join
    // waits at most one retry interval. info must not retry: EmitInfo is also
    // called on the JS thread from Start(), which must never sleep-spin.
    void Emit(DecoderMsg* msg)
    {
        if (tsfn_ == nullptr) {
            delete msg;
            return;
        }
        napi_status st = napi_call_threadsafe_function(tsfn_, msg, napi_tsfn_nonblocking);
        if (st == napi_ok) {
            return;
        }
        if (st == napi_queue_full && (msg->type == MSG_END || msg->type == MSG_ERROR)) {
            for (int i = 0; i < 50 && !stop_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                st = napi_call_threadsafe_function(tsfn_, msg, napi_tsfn_nonblocking);
                if (st == napi_ok) {
                    return;
                }
                if (st != napi_queue_full) {
                    break; // closing: further retries cannot succeed
                }
            }
        }
        if (msg->type != MSG_FRAME) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                "drop tsfn msg type=%{public}d st=%{public}d (queue full or closing)",
                msg->type, static_cast<int>(st));
        }
        delete msg;
    }

    void EmitFrame(double ptsMs)
    {
        // Optional 'frame' event throttle: emit the first frame always, then at
        // most one event per frameEventMs_ window. Apps that only need periodic
        // PTS updates (A/V sync, progress) set frameEventMs to cut the NAPI
        // bridge traffic; 0 (default) keeps the per-frame behavior.
        if (frameEventMs_ > 0) {
            auto now = std::chrono::steady_clock::now();
            if (frameEmitted_ && std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastFrameEmit_).count() < frameEventMs_) {
                return;
            }
            lastFrameEmit_ = now;
            frameEmitted_ = true;
        }
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

    // Unified packet source: the shared demux queue when attached, the private
    // av_read_frame otherwise. Returns av_read_frame-compatible status
    // (0 = ok, <0 = EOF or aborted). `deadlineSteadyMs` (0 = none) bounds a
    // shared-queue pop or arms the private interrupt deadline — used by the
    // hw-eligibility peek so a stream declared but never carrying data cannot
    // block the open path forever.
    int ReadPacket(AVPacket* pkt, int64_t deadlineSteadyMs = 0)
    {
        if (queue_ != nullptr) {
            return queue_->Pop(pkt, stop_, deadlineSteadyMs);
        }
        if (deadlineSteadyMs > 0) {
            peekDeadlineMs_.store(deadlineSteadyMs);
        }
        int ret = av_read_frame(fmt_, pkt);
        if (deadlineSteadyMs > 0) {
            peekDeadlineMs_.store(0);
        }
        return ret;
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
        const char* mime = (codecId == AV_CODEC_ID_HEVC) ? OH_AVCODEC_MIMETYPE_VIDEO_HEVC
                                                         : OH_AVCODEC_MIMETYPE_VIDEO_AVC;
        // Pre-flight capability probe: when the system reports no hardware
        // decoder for this mime, or a size/profile outside its capability
        // range, skip the whole hardware trial (including the first-packet
        // peek) and go straight to the software path.
        int ohProfile = MapOhProfile(codecId, dec_->profile);
        std::string capReason;
        if (!HwVideoDecoder::CheckCapability(mime, width_, height_, fps_, ohProfile, capReason)) {
            lastError_ = capReason;
            if (hwMode_ == HwMode::FORCE) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideo",
                    "hw capability pre-check failed: %{public}s", capReason.c_str());
            } else {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                    "hw capability pre-check failed, using sw: %{public}s", capReason.c_str());
            }
            return false;
        }
        AVPacket* peek = av_packet_alloc();
        bool annexb = false;
        // The peek runs on the JS MAIN thread (napi start is synchronous), so
        // its blocking budget is watchdog-critical. Private path: the context
        // was just opened and its probe buffer already holds the first
        // packets, 3s is only a network-stall guard. Shared path: the queue
        // is fed by the demux thread, which may still be downloading the
        // first segment — cap at 500ms and fall back to software instead of
        // risking a main-thread freeze.
        const int64_t peekDeadline = queue_ != nullptr ? NowMs() + 500 : NowMs() + 3000;
        while (!stop_.load(std::memory_order_relaxed)) {
            int ret = ReadPacket(peek, peekDeadline);
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
                if (!annexb) {
                    lastError_ = "stream is not Annex-B (length-prefixed packets, e.g. MP4)";
                }
                // Keep the first video packet for whichever path ends up
                // running (hw feeds it to the codec, sw sends it first), so
                // the eligibility peek never loses the first keyframe.
                hwPendingPkt_ = peek;
                peek = nullptr;
                break;
            }
            av_packet_unref(peek);
        }
        if (peek != nullptr) {
            av_packet_free(&peek);
        }
        if (!annexb) {
            // hw not eligible; hwPendingPkt_ keeps the first packet for the
            // software path (consumed at the head of the sw decode loop).
            return false;
        }
        hw_ = std::make_unique<HwVideoDecoder>();
        hw_->onFrame = [this](double ptsMs) { EmitFrame(ptsMs); };
        hw_->onEnd = [this]() { EmitEnd(); };
        hw_->onError = [this](const std::string& err) { EmitError(err); };
        // startTimeMs precise start: the render loop releases pre-target
        // output buffers without rendering (codec renders to surface itself)
        hw_->SetDropBeforeUs(dropBeforeMs_ > 0.0 ? static_cast<int64_t>(dropBeforeMs_ * 1000.0) : 0);
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
        if (handle == nullptr || handle->width <= 0 || handle->height <= 0 || handle->stride <= 0) {
            if (mapped) {
                OH_NativeBuffer_Unmap(nb);
            }
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "invalid buffer handle");
            return false;
        }
        // OHOS reports BufferHandle::stride in bytes for RGBA_8888; tolerate
        // devices that report it in pixels (stride < width*4) by re-deriving
        // the byte pitch.
        const int dstPitch = handle->stride >= handle->width * 4 ? handle->stride : handle->width * 4;

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
                if (mapped) {
                    OH_NativeBuffer_Unmap(nb);
                }
                OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
                return false;
            }
        }

        if (handle->width >= frame->width && handle->height >= frame->height) {
            // Fast path: the window buffer covers the frame — convert in place.
            uint8_t* dst[4] = { static_cast<uint8_t*>(virAddr), nullptr, nullptr, nullptr };
            int dstStride[4] = { dstPitch, 0, 0, 0 };
            sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        } else {
            // The window buffer is smaller than the decoded frame: the geometry
            // change is still pending or was rejected (SET_BUFFER_GEOMETRY is
            // negotiated asynchronously with the surface consumer, so the first
            // requested buffers may still carry the old, smaller geometry).
            // Convert into an exact-size scratch buffer and copy row-by-row
            // clamped to the window buffer, so this race can never write out
            // of bounds (was: heap corruption followed by a crash inside
            // jemalloc on the next av_malloc).
            if (!swGeoLogged_) {
                swGeoLogged_ = true;
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                    "window buffer %{public}dx%{public}d smaller than frame %{public}dx%{public}d, clamped copy",
                    handle->width, handle->height, frame->width, frame->height);
            }
            const size_t need = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4;
            if (rgbaScratchCap_ < need) {
                av_free(rgbaScratch_);
                rgbaScratch_ = static_cast<uint8_t*>(av_malloc(need));
                if (rgbaScratch_ == nullptr) {
                    rgbaScratchCap_ = 0;
                    if (mapped) {
                        OH_NativeBuffer_Unmap(nb);
                    }
                    OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
                    return false;
                }
                rgbaScratchCap_ = need;
            }
            uint8_t* dst[4] = { rgbaScratch_, nullptr, nullptr, nullptr };
            int dstStride[4] = { frame->width * 4, 0, 0, 0 };
            sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
            const int copyW = std::min(frame->width, handle->width);
            const int copyH = std::min(frame->height, handle->height);
            const uint8_t* srcRow = rgbaScratch_;
            uint8_t* dstRow = static_cast<uint8_t*>(virAddr);
            for (int y = 0; y < copyH; y++) {
                memcpy(dstRow, srcRow, static_cast<size_t>(copyW) * 4);
                srcRow += static_cast<size_t>(frame->width) * 4;
                dstRow += dstPitch;
            }
        }

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
        // Hold the shared session for the whole worker lifetime: DetachDemux
        // (leak-path cleanup) may drop the core's reference from another
        // thread while this worker is still draining; fmt_ stays valid until
        // this local is destroyed.
        std::shared_ptr<DemuxSession> session = session_;
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
            int ret = ReadPacket(pkt);
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
        // Hold the shared session for the whole worker lifetime (see RunHw).
        std::shared_ptr<DemuxSession> session = session_;
        if (hwActive_.load()) {
            RunHw();
            return;
        }
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool eofSent = false;
        int64_t firstPts = AV_NOPTS_VALUE;
    int dropGuard = 0;
        auto startWall = std::chrono::steady_clock::now();
        bool pausing = false;
        auto pauseStart = std::chrono::steady_clock::now();
        int rendered = 0;
        int failed = 0;
        if (hwPendingPkt_ != nullptr) {
            // packet consumed by the hw eligibility peek when falling back to sw
            avcodec_send_packet(dec_, hwPendingPkt_);
            av_packet_free(&hwPendingPkt_);
        }

        while (!stop_) {
            if (paused_) {
                if (!pausing) {
                    pausing = true;
                    pauseStart = std::chrono::steady_clock::now();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (pausing) {
                // Shift the pacing anchor by the paused span so frames do not
                // burst out to catch up with the wall clock after resume.
                startWall += std::chrono::steady_clock::now() - pauseStart;
                pausing = false;
            }
            int ret = avcodec_receive_frame(dec_, frame);
            if (ret == AVERROR(EAGAIN)) {
                if (eofSent) {
                    EmitEnd();
                    break;
                }
                ret = ReadPacket(pkt);
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

            // startTimeMs precise start: decode but drop pre-target frames.
            // Guard cap protects streams with broken timestamps (all NOPTS →
            // ptsMs 0) from dropping forever.
            if (dropBeforeMs_ > 0.0) {
                if (pts == AV_NOPTS_VALUE || ptsMs < dropBeforeMs_) {
                    av_frame_unref(frame);
                    if (++dropGuard > 1800) {
                        dropBeforeMs_ = 0.0;
                        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                            "pre-target drop guard hit, rendering from current frame");
                    }
                    continue;
                }
                dropBeforeMs_ = 0.0;
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
                if (waitMs >= 1000) {
                    // Forward pts jump (live skip / edge reconnect): re-anchor
                    // the pacing instead of bursting frames at full speed —
                    // a burst desyncs from the (re-anchored) audio pacing.
                    firstPts = pts;
                    startWall = std::chrono::steady_clock::now();
                } else if (waitMs > 0) {
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
    // Shared demux (B1): fmt_ is then BORROWED from the session and must not
    // be closed here; packets come from queue_ instead of av_read_frame.
    std::shared_ptr<DemuxSession> session_;
    std::shared_ptr<PacketQueue> queue_;
    bool sharedFmt_ = false;
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
    // Scratch RGBA buffer for the clamped-copy render fallback (window buffer
    // smaller than the decoded frame, e.g. pending/rejected geometry change).
    uint8_t* rgbaScratch_ = nullptr;
    size_t rgbaScratchCap_ = 0;
    bool swGeoLogged_ = false;
    // 'frame' event throttle (0 = emit every frame)
    int32_t frameEventMs_ = 0;
    bool frameEmitted_ = false;
    std::chrono::steady_clock::time_point lastFrameEmit_ {};
    // startTimeMs precise start: frames with pts below this (ms) are decoded
    // but not rendered/emitted; cleared once the first target frame passes
    double dropBeforeMs_ = 0.0;
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
        napi_create_string_utf8(env, ctx->core->GetColorPrimaries().c_str(), NAPI_AUTO_LENGTH, &v);
        napi_set_named_property(env, result, "colorPrimaries", v);
        napi_create_string_utf8(env, ctx->core->GetColorTransfer().c_str(), NAPI_AUTO_LENGTH, &v);
        napi_set_named_property(env, result, "colorTransfer", v);
        napi_create_string_utf8(env, ctx->core->GetColorRange().c_str(), NAPI_AUTO_LENGTH, &v);
        napi_set_named_property(env, result, "colorRange", v);
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
            "options must be { userAgent?, headers?, allowedExtensions?, liveStartIndex?, analyzeMs?, hw?, program?, httpPersistent?, reconnect? }");
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
        // Detach the shared demux subscription synchronously (queue + claim
        // released; the session reference drops on a helper thread). Waiting
        // for the GC finalizer to run CloseCodec left the subscriber count
        // growing +N per rebuild cycle — a wedged demux session was never
        // released. CloseCodec in the finalizer remains as the codec-free
        // safety net.
        core->DetachDemux();
        core->ReleaseTsfn();
    }
    return nullptr;
}

napi_value FFVideoDecoderFlushToKeyframe(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->FlushToKeyframe();
    }
    return nullptr;
}

napi_value FFVideoDecoderDetach(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->DetachDemux();
    }
    return nullptr;
}

} // namespace ffmpeg
