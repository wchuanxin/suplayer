#include "video_decoder.h"
#include "net_options.h"
#include "hw_decoder.h"
#include "demux_session.h"
#include "hls_playlist.h"
#include "stream_io.h"
#include "error_classify.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
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
    MSG_OVERFLOW = 4,
    MSG_DISCONTINUITY = 5,
    MSG_RECONNECTED = 6,
};

struct DecoderMsg {
    int type = MSG_FRAME;
    double ptsMs = 0.0;
    uint64_t count = 0; // MSG_OVERFLOW: packets dropped in the episode
    std::string err;    // MSG_ERROR: message; MSG_DISCONTINUITY: reason
    std::string backend;
    int64_t num = INT64_MIN; // MSG_DISCONTINUITY: effective seek target (ms)
};

// Completion handshake for seekTo()/captureFrame(): the NAPI async work arms
// the request and blocks on the condition variable; the worker thread applies
// it and notifies. All fields are guarded by mtx.
struct SeekAck {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string err;
};

struct CaptureAck {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string err;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width * height * 4 bytes, RGBA8888
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

// SDR YUV surface gamut (#2 NV12 direct-write): declares the YUV鈫扲GB matrix
// the compositor must assume for semi-planar buffers. HD+ defaults to BT.709,
// SD to BT.601 when the stream leaves the primaries unspecified.
int MapSdrGamut(int primaries, int height)
{
    switch (primaries) {
        case AVCOL_PRI_BT470BG:
        case AVCOL_PRI_BT470M:
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M:
            return NATIVEBUFFER_COLOR_GAMUT_STANDARD_BT601;
        case AVCOL_PRI_BT709:
            return NATIVEBUFFER_COLOR_GAMUT_STANDARD_BT709;
        default:
            return height > 0 && height <= 576 ? NATIVEBUFFER_COLOR_GAMUT_STANDARD_BT601
                                               : NATIVEBUFFER_COLOR_GAMUT_STANDARD_BT709;
    }
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
    napi_ref* OverflowSlot()
    {
        return &overflowRef_;
    }
    napi_ref* DiscontinuitySlot()
    {
        return &discontRef_;
    }
    napi_ref* ReconnectedSlot()
    {
        return &reconnectedRef_;
    }
    // off()/off(event): delete + null every callback reference. JS-thread
    // only (same thread as TsfnDispatcher reads), so no extra locking.
    void ClearAllCallbacks(napi_env env)
    {
        napi_ref* slots[] = { FrameSlot(), EndSlot(), ErrorSlot(), InfoSlot(), OverflowSlot(),
            DiscontinuitySlot(), ReconnectedSlot() };
        for (napi_ref* slot : slots) {
            if (*slot != nullptr) {
                napi_delete_reference(env, *slot);
                *slot = nullptr;
            }
        }
    }
    napi_ref GetOverflowCallback() const
    {
        return overflowRef_;
    }
    napi_ref GetDiscontinuityCallback() const
    {
        return discontRef_;
    }
    napi_ref GetReconnectedCallback() const
    {
        return reconnectedRef_;
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
            // 鍏变韩 demux 浼氳瘽锛氫竴鏉¤繛鎺ユ湇鍔″悓婧愮殑瑙嗛/闊抽/瀛楀箷瑙ｇ爜鍣ㄣ€?
            // open/娴佸垎鏋?program 閿佸畾/startTimeMs seek 鍧囩敱浼氳瘽缁熶竴瀹屾垚锛?
            // 鏈В鐮佸櫒浠呴檮鍔犺闃呭苟淇濈暀鑷繁鐨勭洰鏍囧墠涓㈠抚杩囨护銆?
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
            // A2/F1: variant/latency resolution (infallible 鈥?falls back to
            // the original URL) before the private open.
            const SourceResolution res = ResolveSource(src, netOpts,
                &VideoDecoderCore::InterruptGuard, this);
            NetOptions effOpts = netOpts;
            if (res.liveStartIndex != INT32_MIN) {
                effOpts.liveStartIndex = res.liveStartIndex;
            }
            AVDictionary* dict = nullptr;
            ApplyOpenOptions(fmt_, &dict, effOpts);
            if (res.isHlsPlaylist) {
                ForceHlsInputFormat(fmt_);
            }
            std::string ioErr;
            if (!AttachCustomIO(fmt_, res.url, ioErr)) {
                if (!ioErr.empty()) {
                    lastError_ = ioErr;
                    avformat_free_context(fmt_);
                    fmt_ = nullptr;
                    return false;
                }
            }
            // A failed open frees the context (and the default pb) but never
            // a custom AVIOContext 鈥?keep the pointer to release it below.
            AVIOContext* ioPb = fmt_->pb;
            int ret = avformat_open_input(&fmt_, res.url.c_str(), nullptr, &dict);
            av_dict_free(&dict);
            if (ret < 0) {
                ReleaseStreamIO(ioPb);
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
            // startTimeMs 瀹氫綅璧锋挱锛堢画鎾?seek锛夛細璺冲埌鐩爣鍓嶆渶杩戝叧閿抚锛岃В鐮佸悗鐨?
            // 鐩爣鍓嶅抚鍦ㄦ覆鏌撳惊鐜涪寮冿紝瀹炵幇绮剧‘璧风偣锛涗笉鍙?seek锛堢洿鎾瓑锛夋椂鍥為€€浠庡ご鎾斁
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
        // startTimeMs 鐨勬祦绾?seek 宸插湪涓婃柟绉佹湁璺緞 / 浼氳瘽鍒涘缓鏃跺畬鎴?
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
            queue_->SetStatsStream(streamIndex_);
            // Live video: catch-up overflow policy. A slow (e.g. software
            // decoded 1080p50) video consumer must never block the shared
            // demux thread 鈥?that would starve the audio subscribers.
            BackpressurePolicy policy;
            policy.liveDrop = netOpts.live;
            policy.skipToKeyframe = true;
            queue_->SetPolicy(policy);
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

    // ---- Buffer / playback stats (D4/B2) ------------------------------
    // Media duration of the stats-stream packets currently queued in the
    // shared demux queue; 0 on a private connection (no queue).
    int64_t GetBufferedAheadMs() const
    {
        return queue_ != nullptr ? queue_->BufferedAheadMs() : 0;
    }
    uint64_t GetReadBytes() const
    {
        return readBytes_.load(std::memory_order_relaxed);
    }
    uint64_t GetMeasuredBitrateBps() const
    {
        return measuredBitrateBps_.load(std::memory_order_relaxed);
    }
    uint64_t GetDecodedFrames() const
    {
        return decodedFrames_.load(std::memory_order_relaxed);
    }
    uint64_t GetDroppedFrames() const
    {
        return droppedFrames_.load(std::memory_order_relaxed);
    }
    uint64_t GetOverflowEvents() const
    {
        return queue_ != nullptr ? queue_->OverflowEvents() : 0;
    }
    uint64_t GetReconnectEvents() const
    {
        return queue_ != nullptr ? queue_->ReconnectEvents() : 0;
    }
    uint64_t GetDiscontinuityCount() const
    {
        return discontinuityCount_.load(std::memory_order_relaxed);
    }
    // ---- ABR (1.16) session stats -------------------------------------
    int32_t GetAbrVariantIndex() const
    {
        return session_ != nullptr ? session_->AbrVariantIndex() : -1;
    }
    int64_t GetAbrVariantBandwidthBps() const
    {
        return session_ != nullptr ? session_->AbrVariantBandwidthBps() : 0;
    }
    int32_t GetAbrSwitchCount() const
    {
        return session_ != nullptr ? session_->AbrSwitchCount() : 0;
    }

    // ---- Runtime VOD seek (private + shared 鏂规鈶? ----------------------
    // Arms a seek; the worker applies it at the top of the decode loop
    // (avformat_seek_file -> codec/ pacing reset -> pre-target drop filter)
    // and completes `ack`. Private connection: the worker owns the whole
    // seek. Shared connection: the session's demux thread applies the seek
    // and notifies the queue; the worker re-anchors and completes the ack on
    // the epoch advance. Returns false (with err) when unavailable: not
    // started, dead session, or a private seek still in flight.
    bool RequestSeek(int64_t targetMs, SeekAck* ack, std::string& err)
    {
        if (!opened_ || !running_.load()) {
            err = "decoder is not started";
            return false;
        }
        if (sharedFmt_) {
            // 鏂规鈶?shared-session seek: arm the session (last-write-wins).
            // A still-pending previous shared seek is superseded: its ack
            // resolves immediately, the new target takes over.
            if (session_ == nullptr || queue_ == nullptr) {
                err = "shared session is not attached";
                return false;
            }
            if (!session_->RequestSharedSeek(targetMs)) {
                err = "shared demux session is not alive";
                return false;
            }
            SeekAck* superseded = nullptr;
            {
                std::lock_guard<std::mutex> lock(seekMtx_);
                superseded = seekAck_;
                seekAck_ = ack;
                armedSeekEpoch_ = queue_->SeekEpoch();
            }
            if (superseded != nullptr) {
                FinishSeek(superseded, true, "superseded by a newer seek");
            }
            return true;
        }
        std::lock_guard<std::mutex> lock(seekMtx_);
        if (seekAck_ != nullptr) {
            err = "another seek is already in progress";
            return false;
        }
        seekAck_ = ack;
        pendingSeekMs_.store(targetMs, std::memory_order_release);
        return true;
    }

    // Timeout/stop path: disarm an armed request. When the worker already
    // claimed the ack this is a no-op (the caller must keep the ack alive
    // until the worker completes it).
    void CancelSeekAck(SeekAck* ack)
    {
        std::lock_guard<std::mutex> lock(seekMtx_);
        if (seekAck_ == ack) {
            seekAck_ = nullptr;
            pendingSeekMs_.store(INT64_MIN, std::memory_order_release);
        }
    }

    // ---- Frame capture (software decode path) ----------------------------
    // Arms a one-shot capture of the next decoded frame (after the pre-target
    // drop filter, before render pacing) converted to RGBA at native size.
    bool RequestCapture(CaptureAck* ack, std::string& err)
    {
        if (!opened_ || !running_.load()) {
            err = "decoder is not started";
            return false;
        }
        std::lock_guard<std::mutex> lock(captureMtx_);
        if (captureAck_ != nullptr) {
            err = "another frame capture is already in progress";
            return false;
        }
        captureAck_ = ack;
        captureRequested_.store(true, std::memory_order_release);
        return true;
    }

    void CancelCaptureAck(CaptureAck* ack)
    {
        std::lock_guard<std::mutex> lock(captureMtx_);
        if (captureAck_ == ack) {
            captureAck_ = nullptr;
            captureRequested_.store(false, std::memory_order_release);
        }
    }

    // #2: NV12 direct-write needs even dimensions (h/2 chroma rows), SDR
    // transfer and non-full-range YUV. Everything else keeps the RGBA path.
    bool SwNv12Eligible() const
    {
        if (dec_ == nullptr || (width_ & 1) != 0 || (height_ & 1) != 0) {
            return false;
        }
        if (dec_->color_range == AVCOL_RANGE_JPEG) {
            return false;
        }
        if (dec_->color_trc == AVCOL_TRC_SMPTE2084 || dec_->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            return false;
        }
        switch (dec_->pix_fmt) {
            case AV_PIX_FMT_NV12:
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUV420P10LE:
                return true;
            default:
                return false;
        }
    }

    // Software-path window setup. Prefers NV12 (semi-planar YUV) so decoded
    // frames are written without an RGB conversion pass; falls back to the
    // classic RGBA_8888 when the format is rejected or the content is not
    // NV12-eligible.
    void SetupSwWindow()
    {
        int32_t g1 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, width_, height_);
        swNv12_ = SwNv12Eligible();
        int32_t g2 = -1;
        if (swNv12_) {
            g2 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP);
            if (g2 != 0) {
                swNv12_ = false;
            }
        }
        if (!swNv12_) {
            g2 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
        }
        uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
        int32_t g3 = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_USAGE, usage);
        if (swNv12_ && dec_ != nullptr) {
            // The compositor must know the YUV matrix, not assume one.
            int gamut = MapSdrGamut(dec_->color_primaries, height_);
            int32_t gr = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_COLOR_GAMUT, gamut);
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                "sw window NV12 gamut=%{public}d ret=%{public}d (primaries=%{public}d)",
                gamut, gr, dec_->color_primaries);
        }
        if (g1 != 0 || g2 != 0 || g3 != 0) {
            // Geometry is asynchronously negotiated with the consumer; even a
            // success here does not guarantee the first requested buffers match
            // width_/height_. RenderFrame validates every buffer and falls back
            // to a clamped copy, so a pending/failed geometry change can no
            // longer cause out-of-bounds writes (was: heap corruption crash).
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                "window setup: geometry=%{public}d format=%{public}d usage=%{public}d", g1, g2, g3);
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo", "sw window format=%{public}s",
            swNv12_ ? "NV12" : "RGBA");
    }

    bool Start(uint64_t surfaceId)
    {
        if (!opened_ || window_ != nullptr || running_.load()) {
            return false;
        }
        stop_ = false;
        surfaceId_ = surfaceId;
        hwFailed_.store(false, std::memory_order_release);
        swFallbackArmed_.store(false, std::memory_order_release);
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
            SetupSwWindow();
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

    // User-level playback speed (0.5x-2.0x): the pacing loop (hw render
    // thread or sw Run) divides its pts-delta wall target by the speed.
    // Video frames carry no extra samples to compress, so unlike audio
    // there is no filter pipeline - only the pacing changes; frame pts
    // events stay on the content timeline (same rule as the audio anchor).
    void SetSpeed(double speed)
    {
        if (speed != speed) {
            speed = 1.0; // NaN guard
        } else if (speed < 0.5) {
            speed = 0.5;
        } else if (speed > 2.0) {
            speed = 2.0;
        }
        pendingSpeed_.store(speed, std::memory_order_relaxed);
        if (hw_ != nullptr) {
            hw_->SetSpeed(speed);
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
    // queued keyframe (鈮?live edge). The still-running worker keeps decoding
    // through the gap: the next fed packet is an IDR, so the codec restarts
    // cleanly, and the pacing loop re-anchors on the forward pts jump
    // (waitMs >= 1000), exactly like a native live-overflow skip.
    void FlushToKeyframe()
    {
        if (queue_ == nullptr) {
            return; // private connection: no dropable backlog
        }
        queue_->DropBacklogToKeyframe();
        // The drop shifts the timeline forward: the app's progress anchor is
        // now wrong by the dropped span 鈥?surface it as a discontinuity.
        EmitDiscontinuity("resync");
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
            CancelPendingRequests("decoder stopped");
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        // The worker exited without applying an armed seek/capture: complete
        // it now so a blocked seekTo()/captureFrame() promise resolves.
        CancelPendingRequests("decoder stopped");
    }

    // Complete any still-armed seek/capture with an error (stop path).
    void CancelPendingRequests(const char* reason)
    {
        SeekAck* seekAck = nullptr;
        CaptureAck* captureAck = nullptr;
        {
            std::lock_guard<std::mutex> lock(seekMtx_);
            seekAck = seekAck_;
            seekAck_ = nullptr;
            pendingSeekMs_.store(INT64_MIN, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> lock(captureMtx_);
            captureAck = captureAck_;
            captureAck_ = nullptr;
            captureRequested_.store(false, std::memory_order_release);
        }
        if (seekAck != nullptr) {
            FinishSeek(seekAck, false, reason);
        }
        if (captureAck != nullptr) {
            FinishCapture(captureAck, false, 0, 0, nullptr, reason);
        }
    }

    // Worker side: fetch an armed seek target + its ack (nullptr ack when the
    // request was canceled between arming and claiming).
    int64_t TakePendingSeek(SeekAck** outAck)
    {
        const int64_t target = pendingSeekMs_.exchange(INT64_MIN, std::memory_order_acquire);
        if (target == INT64_MIN) {
            return INT64_MIN;
        }
        std::lock_guard<std::mutex> lock(seekMtx_);
        *outAck = seekAck_;
        seekAck_ = nullptr;
        return target;
    }

    void FinishSeek(SeekAck* ack, bool ok, const std::string& err)
    {
        if (ack == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(ack->mtx);
            ack->done = true;
            ack->ok = ok;
            ack->err = err;
        }
        ack->cv.notify_all();
    }

    // 鏂规鈶?worker side of the shared-session seek: returns true when the
    // queue's seek epoch advanced since the last pass (the demux thread
    // applied a session-wide seek and dropped our backlog). Fills the
    // notified target / success flag and claims the pending shared seek ack
    // when this epoch advance is the one the ack was armed against
    // (epoch > armedSeekEpoch_). The caller re-anchors its path-specific
    // state (codec/hw flush, drop filter, pacing) and completes the ack.
    bool TakeSharedSeek(int64_t& outTargetMs, SeekAck** outAck, bool& outOk)
    {
        *outAck = nullptr;
        if (queue_ == nullptr) {
            return false;
        }
        const uint64_t epoch = queue_->SeekEpoch();
        if (epoch == lastSeekEpochSeen_) {
            return false;
        }
        lastSeekEpochSeen_ = epoch;
        outTargetMs = queue_->SeekTargetMs();
        outOk = queue_->SeekSucceeded();
        std::lock_guard<std::mutex> lock(seekMtx_);
        if (seekAck_ != nullptr && epoch > armedSeekEpoch_) {
            *outAck = seekAck_;
            seekAck_ = nullptr;
        }
        return true;
    }

    // Worker side: fetch an armed capture ack (nullptr when canceled).
    CaptureAck* TakePendingCapture()
    {
        std::lock_guard<std::mutex> lock(captureMtx_);
        CaptureAck* ack = captureAck_;
        captureAck_ = nullptr;
        captureRequested_.store(false, std::memory_order_release);
        return ack;
    }

    void FinishCapture(CaptureAck* ack, bool ok, int width, int height, std::vector<uint8_t>* rgba,
        const std::string& err)
    {
        if (ack == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(ack->mtx);
            ack->done = true;
            ack->ok = ok;
            ack->width = width;
            ack->height = height;
            if (rgba != nullptr) {
                ack->rgba = std::move(*rgba);
            }
            ack->err = err;
        }
        ack->cv.notify_all();
    }

    // Software-path frame capture: convert `frame` to RGBA at native size.
    void CaptureFrameToAck(const AVFrame* frame, CaptureAck* ack)
    {
        if (frame->width <= 0 || frame->height <= 0) {
            FinishCapture(ack, false, 0, 0, nullptr, "invalid frame geometry");
            return;
        }
        // sws_ is normally already configured by RenderFrame; reconfigure
        // defensively (capture may run on the very first frame).
        if (sws_ == nullptr || swsW_ != frame->width || swsH_ != frame->height ||
            swsFmt_ != static_cast<AVPixelFormat>(frame->format) || swsDst_ != AV_PIX_FMT_RGBA) {
            if (sws_ != nullptr) {
                sws_freeContext(sws_);
            }
            swsW_ = frame->width;
            swsH_ = frame->height;
            swsFmt_ = static_cast<AVPixelFormat>(frame->format);
            swsDst_ = AV_PIX_FMT_RGBA;
            sws_ = sws_getContext(swsW_, swsH_, swsFmt_,
                swsW_, swsH_, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws_ == nullptr) {
                FinishCapture(ack, false, 0, 0, nullptr, "sws_getContext failed");
                return;
            }
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4);
        uint8_t* dst[4] = { rgba.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { frame->width * 4, 0, 0, 0 };
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        FinishCapture(ack, true, frame->width, frame->height, &rgba, "");
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
        if (bsf_ != nullptr) {
            av_bsf_free(&bsf_);
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
            AVIOContext* ioPb = fmt_->pb;
            avformat_close_input(&fmt_);
            ReleaseStreamIO(ioPb);
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

    void EmitOverflow(uint64_t dropped)
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_OVERFLOW;
        msg->count = dropped;
        Emit(msg);
    }

    void EmitDiscontinuity(const std::string& reason, int64_t seekMs = INT64_MIN)
    {
        discontinuityCount_.fetch_add(1, std::memory_order_relaxed);
        auto* msg = new DecoderMsg();
        msg->type = MSG_DISCONTINUITY;
        msg->err = reason;
        msg->num = seekMs;
        Emit(msg);
    }

    void EmitReconnected()
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_RECONNECTED;
        Emit(msg);
    }

    // Worker-side polling of the shared queue's overflow counter (F3): the
    // demux thread never calls back into a decoder (a dying decoder's queue
    // would dangle the callback); instead the worker notices a new overflow
    // episode on its next loop pass and emits. Also fires while paused 鈥?
    // a paused live consumer is exactly when the backlog overflows.
    // The reconnect counter (A3) is polled the same way: the session rebuilt
    // its connection in place (fresh CDN token) and dropped our backlog.
    void PollQueueEvents()
    {
        if (queue_ == nullptr) {
            return;
        }
        const uint64_t events = queue_->OverflowEvents();
        if (events != lastOverflowSeen_) {
            const uint64_t droppedTotal = queue_->DroppedPackets();
            EmitOverflow(droppedTotal - lastDroppedSeen_);
            lastOverflowSeen_ = events;
            lastDroppedSeen_ = droppedTotal;
        }
        const uint64_t reconnects = queue_->ReconnectEvents();
        if (reconnects != lastReconnectSeen_) {
            lastReconnectSeen_ = reconnects;
            EmitReconnected();
            EmitDiscontinuity("reconnect");
        }
    }

    // Unified packet source: the shared demux queue when attached, the private
    // av_read_frame otherwise. Returns av_read_frame-compatible status
    // (0 = ok, <0 = EOF or aborted). `deadlineSteadyMs` (0 = none) bounds a
    // shared-queue pop or arms the private interrupt deadline 鈥?used by the
    // hw-eligibility peek so a stream declared but never carrying data cannot
    // block the open path forever.
    int ReadPacket(AVPacket* pkt, int64_t deadlineSteadyMs = 0)
    {
        int ret = -1;
        if (queue_ != nullptr) {
            ret = queue_->Pop(pkt, stop_, deadlineSteadyMs);
        } else {
            if (deadlineSteadyMs > 0) {
                peekDeadlineMs_.store(deadlineSteadyMs);
            }
            ret = av_read_frame(fmt_, pkt);
            if (deadlineSteadyMs > 0) {
                peekDeadlineMs_.store(0);
            }
        }
        if (ret == 0) {
            AccountReadBytes(static_cast<uint64_t>(pkt->size));
        }
        return ret;
    }

    // Byte counter + measured bitrate over a 5s window (D4). Called from the
    // worker thread and, during the hw-eligibility peek, from the JS thread
    // 鈥?never concurrently (the worker is not running while the peek runs).
    void AccountReadBytes(uint64_t bytes)
    {
        readBytes_.fetch_add(bytes, std::memory_order_relaxed);
        const int64_t now = NowMs();
        if (rateWindowStartMs_ == 0) {
            rateWindowStartMs_ = now;
            rateWindowBytes_ = 0;
        }
        rateWindowBytes_ += bytes;
        const int64_t span = now - rateWindowStartMs_;
        if (span >= 5000) {
            const uint64_t instant = static_cast<uint64_t>(
                static_cast<double>(rateWindowBytes_) * 8.0 * 1000.0 / static_cast<double>(span));
            // Blend the just-closed window into the published rate (EWMA) so
            // a single-window spike does not jump the stat.
            const uint64_t prev = measuredBitrateBps_.load(std::memory_order_relaxed);
            const uint64_t blended = prev == 0 ? instant : prev / 2 + instant / 2;
            measuredBitrateBps_.store(blended, std::memory_order_relaxed);
            rateWindowStartMs_ = now;
            rateWindowBytes_ = 0;
        }
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
        // first segment 鈥?cap at 500ms and fall back to software instead of
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
            // Length-prefixed framing (MP4/MKV hvcC/avcC): bring up an
            // mp4toannexb bitstream filter and convert packets on the fly so
            // the hardware path (and with it the HDR color pipeline) covers
            // these containers too. Requires avcC/hvcC extradata; any bsf
            // failure falls back to software exactly as before.
            bool converted = false;
            if (dec_->extradata != nullptr && dec_->extradata_size > 0) {
                const char* bsfName =
                    (codecId == AV_CODEC_ID_H264) ? "h264_mp4toannexb" : "hevc_mp4toannexb";
                const AVBitStreamFilter* bsfFilter = av_bsf_get_by_name(bsfName);
                if (bsfFilter != nullptr && av_bsf_alloc(bsfFilter, &bsf_) >= 0) {
                    avcodec_parameters_from_context(bsf_->par_in, dec_);
                    bsf_->time_base_in = fmt_->streams[streamIndex_]->time_base;
                    if (av_bsf_init(bsf_) >= 0) {
                        converted = true;
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                            "length-prefixed stream: %{public}s conversion active for hw decode", bsfName);
                    } else {
                        av_bsf_free(&bsf_);
                        lastError_ = std::string("bsf init failed: ") + bsfName;
                    }
                }
            }
            if (!converted) {
                // hw not eligible; hwPendingPkt_ keeps the first packet for the
                // software path (consumed at the head of the sw decode loop).
                return false;
            }
            lastError_.clear();
        }
        hw_ = std::make_unique<HwVideoDecoder>();
        hw_->onFrame = [this](double ptsMs) {
            decodedFrames_.fetch_add(1, std::memory_order_relaxed);
            EmitFrame(ptsMs);
        };
        hw_->onEnd = [this]() { EmitEnd(); };
        // #1 mid-stream fallback: in AUTO mode a codec-service error or an
        // input stall no longer bubbles to the app recovery loop 鈥?the hw
        // failure flag reroutes the worker into the already-open software
        // decoder (RunHw 鈫?Run 鈫?SwitchHwToSw). Non-AUTO keeps the old
        // bubble-up behavior.
        hw_->onError = [this](const std::string& err) {
            if (hwMode_ == HwMode::AUTO) {
                hwFailed_.store(true, std::memory_order_release);
            } else {
                EmitError(err);
            }
        };
        // startTimeMs precise start: the render loop releases pre-target
        // output buffers without rendering (codec renders to surface itself)
        hw_->SetDropBeforeUs(dropBeforeMs_ > 0.0 ? static_cast<int64_t>(dropBeforeMs_ * 1000.0) : 0);
        // Replay a pre-start SetSpeed(): hw_ did not exist when the request
        // was stored, and HwVideoDecoder defaults to 1.0x.
        const double spd = pendingSpeed_.load(std::memory_order_relaxed);
        if (spd != 1.0) {
            hw_->SetSpeed(spd);
        }
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

    // #2: per-frame eligibility mirror of SwNv12Eligible (frame metadata can
    // differ from the stream defaults reported at open time).
    bool FrameNv12Eligible(const AVFrame* frame) const
    {
        if ((frame->width & 1) != 0 || (frame->height & 1) != 0) {
            return false;
        }
        if (frame->color_range == AVCOL_RANGE_JPEG) {
            return false;
        }
        if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            return false;
        }
        switch (frame->format) {
            case AV_PIX_FMT_NV12:
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUV420P10LE:
                return true;
            default:
                return false;
        }
    }

    // #2: write the frame into an NV12 (semi-planar YUV) window buffer.
    // Layout resolution: OH_NativeBuffer_MapPlanes is authoritative when it
    // returns a sane semi-planar layout, but some device gralloc HDIs fill
    // it with garbage (observed: planeCount=3, rowStride 1/2 on a 1920-wide
    // NV12 buffer) 鈥?strictly validate it and fall back to the classic
    // BufferHandle-derived layout (Y at 0, UV at stride*height, both planes
    // strided by BufferHandle::stride, which this gralloc reports in bytes
    // as proven by the RGBA path). NV12 frames are copied plane-wise; other
    // eligible YUV formats are converted straight into the mapped planes
    // via swscale (no intermediate buffer).
    bool RenderFrameNv12(const AVFrame* frame, OH_NativeBuffer* nb, int fenceFd, const BufferHandle* handle)
    {
        void* virAddr = nullptr;
        OH_NativeBuffer_Planes planes {};
        bool planesValid = false;
        bool unmapAtEnd = false;
        if (fenceFd >= 0) {
            // MapPlanes has no fence-waiting variant; wait the acquire fence
            // with a throwaway mapping first (CPU-producer path normally has
            // fenceFd < 0).
            void* fenceAddr = nullptr;
            OH_NativeBuffer_MapWaitFence(nb, fenceFd, &fenceAddr);
            OH_NativeBuffer_Unmap(nb);
        }
        if (OH_NativeBuffer_MapPlanes(nb, &virAddr, &planes) == 0 && virAddr != nullptr) {
            const uint64_t yRow = planes.planes[0].rowStride;
            const uint64_t uvRow = planes.planes[1].rowStride;
            const uint64_t uvOff = planes.planes[1].offset;
            const uint64_t need = uvOff + uvRow * (static_cast<uint64_t>(frame->height) / 2);
            planesValid = planes.planeCount == 2 && planes.planes[0].offset == 0 &&
                yRow >= static_cast<uint64_t>(frame->width) &&
                uvRow >= static_cast<uint64_t>(frame->width) &&
                need <= static_cast<uint64_t>(handle->size);
            unmapAtEnd = true;
        }
        size_t yOff = 0;
        size_t uvOff = 0;
        uint32_t yStride = 0;
        uint32_t uvStride = 0;
        if (planesValid) {
            yOff = static_cast<size_t>(planes.planes[0].offset);
            uvOff = static_cast<size_t>(planes.planes[1].offset);
            yStride = planes.planes[0].rowStride;
            uvStride = planes.planes[1].rowStride;
        } else {
            if (unmapAtEnd) {
                OH_NativeBuffer_Unmap(nb);
                unmapAtEnd = false;
            }
            virAddr = nullptr;
            int32_t mapRet = OH_NativeBuffer_Map(nb, &virAddr);
            if (mapRet != 0 || virAddr == nullptr) {
                if (handle->virAddr != nullptr) {
                    virAddr = handle->virAddr;
                } else {
                    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                        "nv12 map failed ret=%{public}d cnt=%{public}u yRow=%{public}u uvRow=%{public}u",
                        mapRet, planes.planeCount, planes.planes[0].rowStride, planes.planes[1].rowStride);
                    return false;
                }
            } else {
                unmapAtEnd = true;
            }
            const size_t stride = static_cast<size_t>(handle->stride);
            const size_t height = static_cast<size_t>(handle->height);
            const size_t uvPlane = stride * ((height + 1) / 2);
            if (stride < static_cast<size_t>(frame->width) || uvPlane == 0 ||
                stride * height + uvPlane > static_cast<size_t>(handle->size)) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideo",
                    "nv12 layout unknowable: stride=%{public}d size=%{public}d cnt=%{public}u",
                    handle->stride, handle->size, planes.planeCount);
                if (unmapAtEnd) {
                    OH_NativeBuffer_Unmap(nb);
                }
                return false;
            }
            yOff = 0;
            // The chroma plane may start at an aligned height (observed on
            // this gralloc: stride * height rounded up to even, matching the
            // MapPlanes-reported offset). Prefer the MapPlanes offset when
            // plausible, then the aligned-height formula, then raw
            // stride*height 鈥?all validated against the buffer size.
            uvOff = stride * height;
            if (planes.planeCount >= 2 && planes.planes[1].offset >= stride * height &&
                planes.planes[1].offset + uvPlane <= static_cast<size_t>(handle->size)) {
                uvOff = static_cast<size_t>(planes.planes[1].offset);
            } else {
                const size_t aligned = stride * ((height + 1) & ~(static_cast<size_t>(1)));
                if (aligned != uvOff && aligned + uvPlane <= static_cast<size_t>(handle->size)) {
                    uvOff = aligned;
                }
            }
            yStride = static_cast<uint32_t>(stride);
            uvStride = static_cast<uint32_t>(stride);
        }
        uint8_t* base = static_cast<uint8_t*>(virAddr);
        uint8_t* yDst = base + yOff;
        uint8_t* uvDst = base + uvOff;
        if (frame->format == AV_PIX_FMT_NV12) {
            for (int r = 0; r < frame->height; r++) {
                memcpy(yDst + r * yStride, frame->data[0] + static_cast<size_t>(r) * frame->linesize[0],
                    static_cast<size_t>(frame->width));
            }
            for (int r = 0; r < frame->height / 2; r++) {
                memcpy(uvDst + r * uvStride, frame->data[1] + static_cast<size_t>(r) * frame->linesize[1],
                    static_cast<size_t>(frame->width));
            }
            if (unmapAtEnd) {
                OH_NativeBuffer_Unmap(nb);
            }
            return true;
        }
        // yuv420p / yuv420p10le 鈫?NV12 straight into the mapped planes
        if (sws_ == nullptr || swsW_ != frame->width || swsH_ != frame->height ||
            swsFmt_ != static_cast<AVPixelFormat>(frame->format) || swsDst_ != AV_PIX_FMT_NV12) {
            if (sws_ != nullptr) {
                sws_freeContext(sws_);
            }
            swsW_ = frame->width;
            swsH_ = frame->height;
            swsFmt_ = static_cast<AVPixelFormat>(frame->format);
            swsDst_ = AV_PIX_FMT_NV12;
            sws_ = sws_getContext(swsW_, swsH_, swsFmt_,
                swsW_, swsH_, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws_ == nullptr) {
                if (unmapAtEnd) {
                    OH_NativeBuffer_Unmap(nb);
                }
                return false;
            }
        }
        uint8_t* dst[4] = { yDst, uvDst, nullptr, nullptr };
        int dstStride[4] = { static_cast<int>(yStride), static_cast<int>(uvStride), 0, 0 };
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        if (unmapAtEnd) {
            OH_NativeBuffer_Unmap(nb);
        }
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
            consecutiveRequestFailures_++;
            lastRequestBufferError_ = ret;
            return false;
        }
        consecutiveRequestFailures_ = 0;

        OH_NativeBuffer* nb = nullptr;
        if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &nb) != 0 || nb == nullptr) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "from native window buffer failed");
            return false;
        }

        BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
        if (handle == nullptr || handle->width <= 0 || handle->height <= 0 || handle->stride <= 0) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "invalid buffer handle");
            return false;
        }

        // #2 NV12 fast path: the window was negotiated as semi-planar YUV 鈥?
        // write through the plane layout without an RGB conversion pass.
        if (swNv12_ && handle->format == NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP &&
            handle->width >= frame->width && handle->height >= frame->height &&
            FrameNv12Eligible(frame)) {
            if (RenderFrameNv12(frame, nb, fenceFd, handle)) {
                Region region = { nullptr, 0 };
                ret = OH_NativeWindow_NativeWindowFlushBuffer(window_, buffer, -1, region);
                if (ret != 0) {
                    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo", "flush failed ret=%{public}d", ret);
                    return false;
                }
                return true;
            }
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
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

        // OHOS reports BufferHandle::stride in bytes for RGBA_8888; tolerate
        // devices that report it in pixels (stride < width*4) by re-deriving
        // the byte pitch.
        const int dstPitch = handle->stride >= handle->width * 4 ? handle->stride : handle->width * 4;

        if (sws_ == nullptr || swsW_ != frame->width || swsH_ != frame->height ||
            swsFmt_ != static_cast<AVPixelFormat>(frame->format) || swsDst_ != AV_PIX_FMT_RGBA) {
            if (sws_ != nullptr) {
                sws_freeContext(sws_);
            }
            swsW_ = frame->width;
            swsH_ = frame->height;
            swsFmt_ = static_cast<AVPixelFormat>(frame->format);
            swsDst_ = AV_PIX_FMT_RGBA;
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
            // Fast path: the window buffer covers the frame 鈥?convert in place.
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
    // Feed one demuxed packet to the hardware decoder, converting through the
    // mp4toannexb filter first when the source is length-prefixed (bsf_ set).
    bool FeedHw(const AVPacket* in, int64_t ptsUs)
    {
        if (bsf_ == nullptr) {
            return hw_->FeedPacket(in->data, in->size, ptsUs, false);
        }
        AVPacket* tmp = av_packet_alloc();
        if (tmp == nullptr) {
            return false;
        }
        if (av_packet_ref(tmp, in) < 0) {
            av_packet_free(&tmp);
            return false;
        }
        bool ok = true;
        if (av_bsf_send_packet(bsf_, tmp) < 0) {
            ok = false;
        } else {
            while (true) {
                const int ret = av_bsf_receive_packet(bsf_, tmp);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    ok = false;
                    break;
                }
                if (!hw_->FeedPacket(tmp->data, tmp->size, ptsUs, false)) {
                    ok = false;
                    break;
                }
                av_packet_unref(tmp);
            }
        }
        av_packet_free(&tmp);
        return ok;
    }

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
            FeedHw(hwPendingPkt_, ptsUs);
            av_packet_free(&hwPendingPkt_);
        }
        while (!stop_) {
            PollQueueEvents();
            // Hot-switch teardown race (see the sw Run loop): a detached queue
            // means this decoder is being torn down. Stop feeding the routed
            // backlog to the codec — the next pipeline is already attaching a
            // new producer to the same surfaceId. Feed EOS (same as the
            // natural EOF path below) so the codec drains and stops.
            if (queue_ != nullptr && queue_->IsDetached()) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                    "queue detached (hw), stop feeding codec");
                hw_->FeedPacket(nullptr, 0, 0, true);
                break;
            }
            // 方案⑤ shared-session seek: the demux thread moved the shared
            // connection and dropped our backlog; re-anchor the hardware
            // path to the notified target and complete the pending ack.
            {
                int64_t sharedTargetMs = 0;
                SeekAck* sharedAck = nullptr;
                bool sharedOk = false;
                if (TakeSharedSeek(sharedTargetMs, &sharedAck, sharedOk)) {
                    if (sharedOk) {
                        hw_->Flush();
                        if (hwPendingPkt_ != nullptr) {
                            av_packet_free(&hwPendingPkt_);
                        }
                        hw_->SetDropBeforeUs(sharedTargetMs * 1000);
                        EmitDiscontinuity("seek", sharedTargetMs);
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                            "seek applied (hw, shared) target=%{public}lldms",
                            static_cast<long long>(sharedTargetMs));
                        FinishSeek(sharedAck, true, "");
                    } else if (sharedAck != nullptr) {
                        FinishSeek(sharedAck, false, "avformat_seek_file failed on shared session");
                    }
                    continue;
                }
            }
            // Runtime VOD seek (private connection): flush the codec, drop
            // the connection to the target keyframe and re-arm the output
            // pre-target filter.
            if (pendingSeekMs_.load(std::memory_order_acquire) != INT64_MIN) {
                SeekAck* seekAck = nullptr;
                const int64_t seekMs = TakePendingSeek(&seekAck);
                if (seekMs != INT64_MIN) {
                    if (hw_ != nullptr) {
                        hw_->Flush();
                    }
                    const int sret = avformat_seek_file(fmt_, -1, INT64_MIN, seekMs * 1000, INT64_MAX, 0);
                    if (sret >= 0) {
                        if (hw_ != nullptr) {
                            hw_->SetDropBeforeUs(seekMs * 1000);
                        }
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                            "seek applied (hw) target=%{public}lldms", static_cast<long long>(seekMs));
                        FinishSeek(seekAck, true, "");
                    } else {
                        FinishSeek(seekAck, false, "avformat_seek_file failed: " + ErrToStr(sret));
                    }
                }
                continue;
            }
            // Frame capture is software-path only; fail armed hw requests.
            if (captureRequested_.load(std::memory_order_acquire)) {
                CaptureAck* captureAck = TakePendingCapture();
                if (captureAck != nullptr) {
                    FinishCapture(captureAck, false, 0, 0, nullptr,
                        "frame capture requires software decode (open with hw: 'never')");
                }
            }
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
                if (!FeedHw(pkt, ptsUs)) {
                    // #1: feed failure = codec error or stalled input. AUTO
                    // mode reroutes this worker into the software path (Run).
                    hwFailed_.store(true, std::memory_order_release);
                    break;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    // #1: hand the worker over from the failed hardware pipeline to the
    // always-open software decoder: stop/destroy the codec, recreate the
    // window (the old surface was consumed by the codec service), flush the
    // decoder and arm the drop-to-keyframe filter so the sw loop resumes at
    // a decodable point. HDR content is excluded 鈥?the sw path cannot
    // tone-map PQ/HLG, so the error surfaces to the app recovery instead.
    bool SwitchHwToSw()
    {
        std::string hwErr = hw_ != nullptr ? hw_->GetLastError() : "";
        if (hw_ != nullptr) {
            hw_->Stop();
            hw_.reset();
        }
        if (dec_ != nullptr && (dec_->color_trc == AVCOL_TRC_SMPTE2084 ||
            dec_->color_trc == AVCOL_TRC_ARIB_STD_B67)) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
                "hw failed mid-stream on HDR content, not falling back to sw: %{public}s", hwErr.c_str());
            EmitError(hwErr.empty() ? "hw decoder failed" : hwErr);
            return false;
        }
        // Live streams: jump the shared backlog to its newest keyframe (live
        // edge) so the switch gap stays one keyframe interval. VOD keeps its
        // backlog 鈥?the sw loop skips only the undecodable mid-GOP packets.
        if (queue_ != nullptr && fmt_ != nullptr &&
            (fmt_->duration == AV_NOPTS_VALUE || fmt_->duration <= 0)) {
            queue_->DropBacklogToKeyframe();
        }
        ReleaseWindow();
        if (surfaceId_ == 0 ||
            OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId_, &window_) != 0 ||
            window_ == nullptr) {
            lastError_ = "recreate window for sw fallback failed";
            OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideo", "%{public}s", lastError_.c_str());
            EmitError(lastError_);
            return false;
        }
        SetupSwWindow();
        avcodec_flush_buffers(dec_);
        swFallbackArmed_.store(true, std::memory_order_release);
        hwActive_.store(false);
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideo",
            "hw decoder failed mid-stream, switched to software decode: %{public}s", hwErr.c_str());
        EmitInfo("sw");
        EmitDiscontinuity("hw2sw");
        return true;
    }

    void Run()
    {
        // Hold the shared session for the whole worker lifetime (see RunHw).
        std::shared_ptr<DemuxSession> session = session_;
        if (hwActive_.load()) {
            RunHw();
            // #1: AUTO-only mid-stream hardware failure 鈥?hand the worker
            // over to the software pipeline instead of ending playback.
            if (hwFailed_.load(std::memory_order_acquire) && !stop_.load() &&
                hwMode_ == HwMode::AUTO && SwitchHwToSw()) {
                hwFailed_.store(false, std::memory_order_release);
                // fall through into the software loop below
            } else {
                return;
            }
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
            PollQueueEvents();
            // Hot-switch teardown race: after the leak-path detach (JS thread
            // never joins), Pop keeps serving the routed backlog, so this
            // worker used to keep rendering into the shared surface for
            // seconds while the next pipeline already attached a new producer
            // to the same surfaceId — the buffer-queue transition freed the
            // in-flight OH_NativeBuffer mid-render and crashed inside
            // OH_NativeBuffer_Unmap (fault thread = this worker). A detached
            // queue means this decoder is being torn down: exit now, no more
            // surface touches.
            if (queue_ != nullptr && queue_->IsDetached()) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                    "queue detached (sw), exit without surface drain");
                EmitEnd();
                break;
            }
            // 鏂规鈥?shared-session seek: the demux thread moved the shared
            // connection and dropped our backlog; re-anchor the software
            // path (decoder flush, pacing, pre-target drop filter) to the
            // notified target and complete the pending ack.
            {
                int64_t sharedTargetMs = 0;
                SeekAck* sharedAck = nullptr;
                bool sharedOk = false;
                if (TakeSharedSeek(sharedTargetMs, &sharedAck, sharedOk)) {
                    if (sharedOk) {
                        avcodec_flush_buffers(dec_);
                        av_frame_unref(frame);
                        dropBeforeMs_ = static_cast<double>(sharedTargetMs);
                        dropGuard = 0;
                        firstPts = AV_NOPTS_VALUE;
                        startWall = std::chrono::steady_clock::now();
                        eofSent = false;
                        EmitDiscontinuity("seek", sharedTargetMs);
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                            "seek applied (sw, shared) target=%{public}lldms",
                            static_cast<long long>(sharedTargetMs));
                        FinishSeek(sharedAck, true, "");
                    } else if (sharedAck != nullptr) {
                        FinishSeek(sharedAck, false, "avformat_seek_file failed on shared session");
                    }
                    continue;
                }
            }
            // Runtime VOD seek (private connection): flush the decoder, seek
            // the connection to the target keyframe, re-anchor pacing and
            // re-arm the pre-target drop filter so playback resumes exactly
            // at the requested position.
            if (pendingSeekMs_.load(std::memory_order_acquire) != INT64_MIN) {
                SeekAck* seekAck = nullptr;
                const int64_t seekMs = TakePendingSeek(&seekAck);
                if (seekMs != INT64_MIN) {
                    avcodec_flush_buffers(dec_);
                    av_frame_unref(frame);
                    const int sret = avformat_seek_file(fmt_, -1, INT64_MIN, seekMs * 1000, INT64_MAX, 0);
                    if (sret >= 0) {
                        dropBeforeMs_ = static_cast<double>(seekMs);
                        dropGuard = 0;
                        firstPts = AV_NOPTS_VALUE;
                        startWall = std::chrono::steady_clock::now();
                        eofSent = false;
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideo",
                            "seek applied (sw) target=%{public}lldms", static_cast<long long>(seekMs));
                        FinishSeek(seekAck, true, "");
                    } else {
                        FinishSeek(seekAck, false, "avformat_seek_file failed: " + ErrToStr(sret));
                    }
                }
                continue;
            }
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
                    // #1 hw2sw switch: the flushed decoder cannot resume
                    // mid-GOP 鈥?drop packets until the next keyframe.
                    if (swFallbackArmed_.load(std::memory_order_acquire)) {
                        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
                            av_packet_unref(pkt);
                            continue;
                        }
                        swFallbackArmed_.store(false, std::memory_order_release);
                    }
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
            // Guard cap protects streams with broken timestamps (all NOPTS 鈫?
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

            // One-shot frame capture: convert this frame to RGBA at native
            // size (before render pacing, so the promise resolves promptly).
            if (captureRequested_.load(std::memory_order_acquire)) {
                CaptureAck* captureAck = TakePendingCapture();
                if (captureAck != nullptr) {
                    CaptureFrameToAck(frame, captureAck);
                }
            }

            // real-time pacing based on pts delta
            if (pendingSpeed_.load(std::memory_order_relaxed) != activeSpeed_) {
                // Speed change: re-anchor the pacing at this frame so the old
                // anchor cannot burst or stall the new speed's timeline.
                activeSpeed_ = pendingSpeed_.load(std::memory_order_relaxed);
                firstPts = AV_NOPTS_VALUE;
            }
            if (firstPts == AV_NOPTS_VALUE) {
                firstPts = pts;
                startWall = std::chrono::steady_clock::now();
            } else if (pts != AV_NOPTS_VALUE) {
                // speed divides the wall target: content delta D renders in
                // D/speed wall seconds
                double targetMs = static_cast<double>(pts - firstPts) * av_q2d(stream->time_base) * 1000.0 /
                    (activeSpeed_ > 0.0 ? activeSpeed_ : 1.0);
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startWall).count();
                int64_t waitMs = static_cast<int64_t>(targetMs) - static_cast<int64_t>(elapsed);
                if (waitMs >= 1000) {
                    // Forward pts jump (live skip / edge reconnect): re-anchor
                    // the pacing instead of bursting frames at full speed 鈥?
                    // a burst desyncs from the (re-anchored) audio pacing.
                    firstPts = pts;
                    startWall = std::chrono::steady_clock::now();
                } else if (waitMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                }
            }

            if (RenderFrame(frame)) {
                rendered++;
                decodedFrames_.fetch_add(1, std::memory_order_relaxed);
            } else {
                failed++;
                droppedFrames_.fetch_add(1, std::memory_order_relaxed);
                // Surface wedge fail-fast: a poisoned BufferQueue (lingering
                // codec-service producer after hw teardown) fails every
                // NativeWindowRequestBuffer with 41206000 forever — spinning
                // the decode loop at full speed only floods the log (observed
                // 28 min / 34k lines / 85% CPU) and never recovers. Bail out
                // after ~1s of consecutive request failures and hand recovery
                // to the app's reload/surface-rebuild chain.
                if (consecutiveRequestFailures_ >= RENDER_REQUEST_FAIL_BAILOUT) {
                    const int err = lastRequestBufferError_;
                    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideo",
                        "surface wedged: %{public}d consecutive request buffer failures (ret=%{public}d), bailing out",
                        consecutiveRequestFailures_, err);
                    EmitError("video surface wedged: request buffer failed (ret=" +
                        std::to_string(err) + "), needs surface rebuild");
                    break;
                }
            }
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
    // Surface-wedge fail-fast (see Run): consecutive NativeWindowRequestBuffer
    // failures and the last error code; reset on any successful request.
    static constexpr int RENDER_REQUEST_FAIL_BAILOUT = 30;
    int consecutiveRequestFailures_ = 0;
    int lastRequestBufferError_ = 0;
    OHNativeWindow* window_ = nullptr;
    std::unique_ptr<HwVideoDecoder> hw_;
    int streamIndex_ = -1;
    int width_ = 0;
    int height_ = 0;
    int swsW_ = 0;
    int swsH_ = 0;
    AVPixelFormat swsFmt_ = AV_PIX_FMT_NONE;
    // Destination of the cached sws_ context (RGBA render / NV12 direct-write /
    // RGBA capture) 鈥?part of the cache key so routes never share a context.
    AVPixelFormat swsDst_ = AV_PIX_FMT_NONE;
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
    // #1 mid-stream hw鈫抯w fallback: set by hw onError/FeedHw failure in AUTO
    // mode; Run() switches the worker to the software pipeline once.
    std::atomic<bool> hwFailed_{ false };
    // Drop packets until the next keyframe after a hw2sw switch (flushed
    // decoder cannot resume mid-GOP).
    std::atomic<bool> swFallbackArmed_{ false };
    // SurfaceId captured in Start() 鈥?the fallback recreates the window from
    // it after the codec consumed the old surface.
    uint64_t surfaceId_ = 0;
    // #2: window negotiated as NV12 (semi-planar YUV) for the software path.
    bool swNv12_ = false;
    AVPacket* hwPendingPkt_ = nullptr;
    // mp4toannexb filter for length-prefixed (hvcC/avcC) sources on the
    // hardware path; nullptr for native Annex-B streams.
    AVBSFContext* bsf_ = nullptr;
    std::atomic<int64_t> peekDeadlineMs_{ 0 };
    bool opened_ = false;
    std::string lastError_;

    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> paused_{ false };

    // ---- Runtime seek (private + shared 鏂规鈶? ----
    std::atomic<int64_t> pendingSeekMs_{ INT64_MIN }; // INT64_MIN = none armed
    SeekAck* seekAck_ = nullptr;                     // guarded by seekMtx_
    std::mutex seekMtx_;
    // Shared seek: queue epoch snapshot taken when the ack was armed
    // (seekMtx_); the worker completes the ack once the queue epoch passes it.
    uint64_t armedSeekEpoch_ = 0;
    // Worker-only mirror of queue_->SeekEpoch() (advances past it on a
    // session seek); 0 matches a fresh queue before any seek.
    uint64_t lastSeekEpochSeen_ = 0;
    // ---- User-level speed (0.5x-2.0x) ----
    // NAPI stores the request; the hw render thread (HwVideoDecoder) or the
    // sw Run pacing applies it by dividing the wall target.
    std::atomic<double> pendingSpeed_{ 1.0 };
    double activeSpeed_ = 1.0; // sw Run worker-only mirror

    // ---- Frame capture (software path) ----
    std::atomic<bool> captureRequested_{ false };
    CaptureAck* captureAck_ = nullptr;               // guarded by captureMtx_
    std::mutex captureMtx_;

    napi_threadsafe_function tsfn_ = nullptr;
    napi_ref frameRef_ = nullptr;
    napi_ref endRef_ = nullptr;
    napi_ref errorRef_ = nullptr;
    napi_ref infoRef_ = nullptr;
    napi_ref overflowRef_ = nullptr;
    napi_ref discontRef_ = nullptr;
    napi_ref reconnectedRef_ = nullptr;

    // Buffer / playback stats (D4/B2/F3)
    std::atomic<uint64_t> readBytes_{ 0 };
    std::atomic<uint64_t> measuredBitrateBps_{ 0 };
    int64_t rateWindowStartMs_ = 0;
    uint64_t rateWindowBytes_ = 0;
    std::atomic<uint64_t> decodedFrames_{ 0 };
    std::atomic<uint64_t> droppedFrames_{ 0 };
    std::atomic<uint64_t> discontinuityCount_{ 0 };
    uint64_t lastOverflowSeen_ = 0;
    uint64_t lastDroppedSeen_ = 0;
    uint64_t lastReconnectSeen_ = 0;
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
    } else if (msg->type == MSG_OVERFLOW) {
        if (core != nullptr && core->GetOverflowCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetOverflowCallback(), &cb);
            if (cb != nullptr) {
                napi_value ev = nullptr;
                napi_create_object(env, &ev);
                napi_value dropped = nullptr;
                napi_create_int64(env, static_cast<int64_t>(msg->count), &dropped);
                napi_set_named_property(env, ev, "dropped", dropped);
                napi_call_function(env, undefined, cb, 1, &ev, &undefined);
            }
        }
    } else if (msg->type == MSG_DISCONTINUITY) {
        if (core != nullptr && core->GetDiscontinuityCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetDiscontinuityCallback(), &cb);
            if (cb != nullptr) {
                napi_value ev = nullptr;
                napi_create_object(env, &ev);
                napi_value reason = nullptr;
                napi_create_string_utf8(env, msg->err.c_str(), NAPI_AUTO_LENGTH, &reason);
                napi_set_named_property(env, ev, "reason", reason);
                if (msg->num != INT64_MIN) {
                    napi_value seekMs = nullptr;
                    napi_create_int64(env, msg->num, &seekMs);
                    napi_set_named_property(env, ev, "seekMs", seekMs);
                }
                napi_call_function(env, undefined, cb, 1, &ev, &undefined);
            }
        }
    } else if (msg->type == MSG_RECONNECTED) {
        // A3: the shared session rebuilt its connection (fresh CDN token);
        // no payload 鈥?poll getBufferStats() for counters.
        if (core != nullptr && core->GetReconnectedCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetReconnectedCallback(), &cb);
            if (cb != nullptr) {
                napi_call_function(env, undefined, cb, 0, nullptr, &undefined);
            }
        }
    } else if (msg->type == MSG_ERROR) {
        if (core != nullptr && core->GetErrorCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetErrorCallback(), &cb);
            if (cb != nullptr) {
                CallErrorCallback(env, undefined, cb, msg->err);
            }
        }
    }
    delete msg;
}

void CoreFinalize(napi_env env, void* data, void* hint)
{
    (void)hint;
    auto* core = static_cast<VideoDecoderCore*>(data);
    // The JS object died without close(): unsubscribe the shared demux
    // session BEFORE the delete 鈥?a zombie queue keeps receiving routed
    // packets until the destructor runs (endless high-water drop storm, and
    // the demux thread can sit blocked on the queue's full mark while the
    // queue object is being destroyed).
    core->DetachDemux();
    core->ReleaseTsfn();
    core->ClearAllCallbacks(env);
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
            "options must be { userAgent?, headers?, allowedExtensions?, liveStartIndex?, analyzeMs?, hw?, program?, httpPersistent?, reconnect?, variantIndex?, maxBandwidth?, abr?, targetLatencyMs?, autoReconnect?, reconnectMaxRetries?, dvrSeek?, rtspTransport?, bufferSize?, rwTimeoutMs? }");
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
    } else if (event == "overflow") {
        slot = core->OverflowSlot();
    } else if (event == "discontinuity") {
        slot = core->DiscontinuitySlot();
    } else if (event == "reconnected") {
        slot = core->ReconnectedSlot();
    } else {
        napi_throw_type_error(env, nullptr,
            "unsupported event, expect 'frame' | 'end' | 'error' | 'info' | 'overflow' | 'discontinuity' | 'reconnected'");
        return nullptr;
    }
    if (*slot != nullptr) {
        napi_delete_reference(env, *slot);
    }
    napi_create_reference(env, args[1], 1, slot);
    return nullptr;
}

napi_value FFVideoDecoderOff(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    // off() with no event: remove all listeners (removeAllListeners).
    if (argc == 0 || args[0] == nullptr) {
        core->ClearAllCallbacks(env);
        return nullptr;
    }
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[0], &type);
    if (type == napi_undefined || type == napi_null) {
        core->ClearAllCallbacks(env);
        return nullptr;
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) {
        napi_throw_type_error(env, nullptr, "off(event?: string) expects an event name or no argument");
        return nullptr;
    }
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
    } else if (event == "overflow") {
        slot = core->OverflowSlot();
    } else if (event == "discontinuity") {
        slot = core->DiscontinuitySlot();
    } else if (event == "reconnected") {
        slot = core->ReconnectedSlot();
    } else {
        napi_throw_type_error(env, nullptr,
            "unsupported event, expect 'frame' | 'end' | 'error' | 'info' | 'overflow' | 'discontinuity' | 'reconnected'");
        return nullptr;
    }
    if (*slot != nullptr) {
        napi_delete_reference(env, *slot);
        *slot = nullptr;
    }
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
        // growing +N per rebuild cycle 鈥?a wedged demux session was never
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

napi_value FFVideoDecoderSetSpeed(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "setSpeed(speed: number) requires a speed argument");
        return nullptr;
    }
    double speed = 1.0;
    napi_get_value_double(env, args[0], &speed);
    core->SetSpeed(speed);
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

napi_value FFVideoDecoderGetBufferStats(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    napi_value result = nullptr;
    napi_create_object(env, &result);
    napi_value v = nullptr;
    napi_create_int64(env, core->GetBufferedAheadMs(), &v);
    napi_set_named_property(env, result, "bufferedAheadMs", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetReadBytes()), &v);
    napi_set_named_property(env, result, "readBytes", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetMeasuredBitrateBps()), &v);
    napi_set_named_property(env, result, "measuredBitrateBps", v);
    // Live latency needs playlist-level timing (LL-HLS / PVR-DVR); -1 until wired.
    napi_create_int64(env, -1, &v);
    napi_set_named_property(env, result, "liveLatencyMs", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetOverflowEvents()), &v);
    napi_set_named_property(env, result, "overflowCount", v);
    // A3: in-place session rebuilds (token self-healing), from the queue.
    napi_create_int64(env, static_cast<int64_t>(core->GetReconnectEvents()), &v);
    napi_set_named_property(env, result, "reconnectCount", v);
    // ABR (1.16): active HLS variant (doc index) + bandwidth; -1/0 on a
    // private connection or when no master-playlist variant was selected.
    napi_create_int32(env, core->GetAbrVariantIndex(), &v);
    napi_set_named_property(env, result, "variantIndex", v);
    napi_create_int64(env, core->GetAbrVariantBandwidthBps(), &v);
    napi_set_named_property(env, result, "variantBandwidthBps", v);
    napi_create_int32(env, core->GetAbrSwitchCount(), &v);
    napi_set_named_property(env, result, "variantSwitchCount", v);
    return result;
}

napi_value FFVideoDecoderGetStats(napi_env env, napi_callback_info info)
{
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    napi_value result = nullptr;
    napi_create_object(env, &result);
    napi_value v = nullptr;
    napi_create_int64(env, static_cast<int64_t>(core->GetDecodedFrames()), &v);
    napi_set_named_property(env, result, "decodedFrames", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetDroppedFrames()), &v);
    napi_set_named_property(env, result, "droppedFrames", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetOverflowEvents()), &v);
    napi_set_named_property(env, result, "overflowCount", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetReconnectEvents()), &v);
    napi_set_named_property(env, result, "reconnectCount", v);
    napi_create_int64(env, static_cast<int64_t>(core->GetDiscontinuityCount()), &v);
    napi_set_named_property(env, result, "discontinuityCount", v);
    return result;
}

namespace {

// seekTo()/captureFrame() request contexts: the Execute arms the request on
// the core and blocks on the ack condvar until the worker applies it; the
// Complete resolves/rejects the promise.
struct VideoSeekContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    VideoDecoderCore* core = nullptr;
    int64_t targetMs = 0;
    SeekAck ack;
    bool armed = false;
    bool timedOut = false;
};

void VideoSeekExecute(napi_env env, void* data)
{
    auto* ctx = static_cast<VideoSeekContext*>(data);
    std::string err;
    if (!ctx->core->RequestSeek(ctx->targetMs, &ctx->ack, err)) {
        ctx->ack.err = err;
        return;
    }
    ctx->armed = true;
    std::unique_lock<std::mutex> lock(ctx->ack.mtx);
    if (!ctx->ack.cv.wait_for(lock, std::chrono::seconds(15), [&ctx] { return ctx->ack.done; })) {
        ctx->timedOut = true;
        // Disarm when the worker has not claimed the ack yet; when it has
        // (e.g. blocked in a slow network seek), Complete keeps the context
        // alive until the worker finishes it.
        ctx->core->CancelSeekAck(&ctx->ack);
    }
}

void VideoSeekComplete(napi_env env, napi_status status, void* data)
{
    auto* ctx = static_cast<VideoSeekContext*>(data);
    if (ctx->timedOut) {
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, "seek timed out", NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
        napi_delete_async_work(env, ctx->work);
        if (!ctx->ack.done) {
            std::thread([ctx]() {
                std::unique_lock<std::mutex> lock(ctx->ack.mtx);
                ctx->ack.cv.wait_for(lock, std::chrono::seconds(60), [&ctx] { return ctx->ack.done; });
                delete ctx;
            }).detach();
            return;
        }
        delete ctx;
        return;
    }
    if (!ctx->armed || !ctx->ack.ok) {
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, ctx->ack.err.c_str(), NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
    } else {
        napi_value undefined = nullptr;
        napi_get_undefined(env, &undefined);
        napi_resolve_deferred(env, ctx->deferred, undefined);
    }
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

struct VideoCaptureContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    VideoDecoderCore* core = nullptr;
    CaptureAck ack;
    bool armed = false;
    bool timedOut = false;
};

void VideoCaptureExecute(napi_env env, void* data)
{
    auto* ctx = static_cast<VideoCaptureContext*>(data);
    std::string err;
    if (!ctx->core->RequestCapture(&ctx->ack, err)) {
        ctx->ack.err = err;
        return;
    }
    ctx->armed = true;
    std::unique_lock<std::mutex> lock(ctx->ack.mtx);
    if (!ctx->ack.cv.wait_for(lock, std::chrono::seconds(15), [&ctx] { return ctx->ack.done; })) {
        ctx->timedOut = true;
        ctx->core->CancelCaptureAck(&ctx->ack);
    }
}

void VideoCaptureComplete(napi_env env, napi_status status, void* data)
{
    auto* ctx = static_cast<VideoCaptureContext*>(data);
    if (ctx->timedOut) {
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, "frame capture timed out", NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
        napi_delete_async_work(env, ctx->work);
        if (!ctx->ack.done) {
            std::thread([ctx]() {
                std::unique_lock<std::mutex> lock(ctx->ack.mtx);
                ctx->ack.cv.wait_for(lock, std::chrono::seconds(60), [&ctx] { return ctx->ack.done; });
                delete ctx;
            }).detach();
            return;
        }
        delete ctx;
        return;
    }
    if (!ctx->armed || !ctx->ack.ok) {
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, ctx->ack.err.c_str(), NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
    } else {
        napi_value result = nullptr;
        napi_create_object(env, &result);
        napi_value v = nullptr;
        napi_create_int32(env, ctx->ack.width, &v);
        napi_set_named_property(env, result, "width", v);
        napi_create_int32(env, ctx->ack.height, &v);
        napi_set_named_property(env, result, "height", v);
        napi_value ab = nullptr;
        void* abData = nullptr;
        napi_create_arraybuffer(env, ctx->ack.rgba.size(), &abData, &ab);
        if (abData != nullptr && !ctx->ack.rgba.empty()) {
            memcpy(abData, ctx->ack.rgba.data(), ctx->ack.rgba.size());
        }
        napi_set_named_property(env, result, "data", ab);
        napi_resolve_deferred(env, ctx->deferred, result);
    }
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

} // namespace

napi_value FFVideoDecoderSeekTo(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, &argc, args, &thisVar, nullptr);
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "seekTo(targetMs: number) requires targetMs");
        return nullptr;
    }
    double target = 0.0;
    if (napi_get_value_double(env, args[0], &target) != napi_ok || target < 0.0) {
        napi_throw_type_error(env, nullptr, "targetMs must be a non-negative number");
        return nullptr;
    }
    auto* ctx = new VideoSeekContext();
    ctx->core = core;
    ctx->targetMs = static_cast<int64_t>(target);
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_video_seek", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, thisVar, resourceName, VideoSeekExecute, VideoSeekComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value FFVideoDecoderCaptureFrame(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, &argc, args, &thisVar, nullptr);
    VideoDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFVideoDecoder is not constructed");
        return nullptr;
    }
    (void)args;
    auto* ctx = new VideoCaptureContext();
    ctx->core = core;
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_video_capture", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, thisVar, resourceName, VideoCaptureExecute, VideoCaptureComplete, ctx,
        &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

} // namespace ffmpeg
