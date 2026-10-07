#include "subtitle_decoder.h"
#include "net_options.h"
#include "demux_session.h"
#include "hls_playlist.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <hilog/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ffmpeg {
namespace {

enum SubMsgType {
    SUBMSG_SUBTITLE = 0,
    SUBMSG_END = 1,
    SUBMSG_ERROR = 2,
    // raw PES passthrough (DVB pump mode): payload + pts pushed to JS as-is
    SUBMSG_PES = 3,
    SUBMSG_OVERFLOW = 4,
    SUBMSG_DISCONTINUITY = 5,
};

// One decoded subtitle display set, composited into a single RGBA bitmap that
// covers the union bounding box of all bitmap regions (usually a thin strip).
struct SubMsg {
    int type = SUBMSG_SUBTITLE;
    bool clear = false;
    double ptsMs = 0.0;
    double durationMs = 0.0;
    int32_t canvasWidth = 0;
    int32_t canvasHeight = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t regionCount = 0;
    uint8_t* rgba = nullptr;
    size_t rgbaLen = 0;
    std::string text;
    std::string err;
    // SUBMSG_PES: demuxed subtitle packet payload (PES from data_identifier)
    // + pts in stream time_base units (90kHz for MPEG-TS, -1 = none)
    uint8_t* pes = nullptr;
    size_t pesLen = 0;
    double pesPts = -1.0;
    uint64_t count = 0; // SUBMSG_OVERFLOW: packets dropped in the episode
};

struct RgbaRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    const AVSubtitleRect* rect = nullptr;
};

std::string ErrToStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

std::string DictGet(AVDictionary* dict, const char* key)
{
    const AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
    return (e != nullptr && e->value != nullptr) ? std::string(e->value) : std::string();
}

int InterruptOnStop(void* opaque)
{
    auto* flag = static_cast<std::atomic<bool>*>(opaque);
    return flag->load(std::memory_order_relaxed) ? 1 : 0;
}

class SubtitleDecoderCore {
public:
    SubtitleDecoderCore() = default;
    ~SubtitleDecoderCore()
    {
        StopInternal();
        CloseCodec();
    }

    napi_ref* SubtitleSlot()
    {
        return &subtitleRef_;
    }
    napi_ref* EndSlot()
    {
        return &endRef_;
    }
    napi_ref* ErrorSlot()
    {
        return &errorRef_;
    }
    napi_ref* PesSlot()
    {
        return &pesRef_;
    }
    napi_ref* OverflowSlot()
    {
        return &overflowRef_;
    }
    napi_ref* DiscontinuitySlot()
    {
        return &discontinuityRef_;
    }
    napi_ref GetSubtitleCallback() const
    {
        return subtitleRef_;
    }
    napi_ref GetEndCallback() const
    {
        return endRef_;
    }
    napi_ref GetErrorCallback() const
    {
        return errorRef_;
    }
    napi_ref GetPesCallback() const
    {
        return pesRef_;
    }
    napi_ref GetOverflowCallback() const
    {
        return overflowRef_;
    }
    napi_ref GetDiscontinuityCallback() const
    {
        return discontinuityRef_;
    }
    // Raw PES pump mode: skip avcodec decoding entirely and forward subtitle
    // packets as-is. Must be set before start(); open() runs unchanged (the
    // codec context is opened but unused in raw mode).
    void SetRawPump(bool raw)
    {
        raw_ = raw;
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

    // Opens the container and selects a subtitle stream. The stream only needs
    // to be *declared* by the container (e.g. a DVB subtitle entry in the PMT):
    // opening succeeds even when the subtitle PID never carries data, so the
    // feature can be enabled straight from the program table.
    bool Open(const std::string& src, int streamIndex, const NetOptions& netOpts)
    {
        CloseCodec();
        stop_ = false;
        AVProgram* program = nullptr;
        if (netOpts.sharedDemux) {
            // 共享 demux 会话：一条连接服务同源的全部解码器；open/流分析/
            // program 锁定均由会话统一完成（字幕不参与 startTimeMs seek，
            // 应用层以当前播放位置起拉二级会话）
            std::string err;
            session_ = DemuxSession::GetOrCreate(src, netOpts, err);
            if (session_ == nullptr) {
                lastError_ = "shared demux open failed: " + err;
                return false;
            }
            fmt_ = session_->fmt(); // borrowed: closed by the session
            sharedFmt_ = true;
            program = session_->program();
        } else {
            sharedFmt_ = false;
            fmt_ = avformat_alloc_context();
            fmt_->interrupt_callback.callback = InterruptOnStop;
            fmt_->interrupt_callback.opaque = &stop_;
            // A2/F1: variant/latency resolution (infallible — falls back to
            // the original URL) before the private open.
            const SourceResolution res = ResolveSource(src, netOpts, &InterruptOnStop, &stop_);
            NetOptions effOpts = netOpts;
            if (res.liveStartIndex != INT32_MIN) {
                effOpts.liveStartIndex = res.liveStartIndex;
            }
            AVDictionary* dict = nullptr;
            ApplyOpenOptions(fmt_, &dict, effOpts);
            int ret = avformat_open_input(&fmt_, res.url.c_str(), nullptr, &dict);
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
        }
        if (streamIndex < 0) {
            if (program != nullptr) {
                streamIndex = FindStreamInProgram(fmt_, program, AVMEDIA_TYPE_SUBTITLE);
            } else {
                streamIndex = av_find_best_stream(fmt_, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
            }
            if (streamIndex < 0) {
                lastError_ = "no subtitle stream declared";
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
        AVStream* stream = fmt_->streams[streamIndex];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            lastError_ = "stream is not a subtitle stream";
            CloseCodec();
            return false;
        }
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            lastError_ = "unsupported subtitle codec";
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
        codecName_ = codec->name != nullptr ? codec->name : "";
        language_ = DictGet(stream->metadata, "language");
        streamIndex_ = streamIndex;
        program_ = program;
        canvasWidth_ = dec_->width > 0 ? dec_->width : 0;
        canvasHeight_ = dec_->height > 0 ? dec_->height : 0;
        if (canvasWidth_ <= 0 || canvasHeight_ <= 0) {
            // DVB subtitles carry their composition size in a display definition
            // segment; fall back to the paired video size until one arrives.
            int videoIndex = program != nullptr
                ? FindStreamInProgram(fmt_, program, AVMEDIA_TYPE_VIDEO)
                : av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (videoIndex >= 0 && static_cast<unsigned int>(videoIndex) < fmt_->nb_streams) {
                canvasWidth_ = fmt_->streams[videoIndex]->codecpar->width;
                canvasHeight_ = fmt_->streams[videoIndex]->codecpar->height;
            }
        }
        declared_ = true;
        if (sharedFmt_ && session_ != nullptr) {
            queue_ = session_->Subscribe({ streamIndex_ });
            queue_->SetStatsStream(streamIndex_);
            // Live catch-up overflow policy, same as audio: a subtitle queue
            // that never pops (no track selected by the app) must not
            // backpressure the shared demux read into a permanent wedge once
            // the pre-start cap fills — the demux thread would block forever
            // in Route() and starve every other subscriber of the session.
            BackpressurePolicy policy;
            policy.liveDrop = netOpts.live;
            queue_->SetPolicy(policy);
        }
        opened_ = true;
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFSub",
            "subtitle opened stream=#%{public}d codec=%{public}s lang=%{public}s canvas=%{public}dx%{public}d declared=%{public}d",
            streamIndex_, codecName_.c_str(), language_.c_str(), canvasWidth_, canvasHeight_, declared_ ? 1 : 0);
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
    int64_t GetDurationMs() const
    {
        if (!opened_ || fmt_ == nullptr) {
            return 0;
        }
        return fmt_->duration > 0 ? fmt_->duration / (AV_TIME_BASE / 1000) : 0;
    }
    std::string GetCodecName() const
    {
        return codecName_;
    }
    std::string GetLanguage() const
    {
        return language_;
    }
    int32_t GetCanvasWidth() const
    {
        return canvasWidth_;
    }
    int32_t GetCanvasHeight() const
    {
        return canvasHeight_;
    }
    int32_t GetStreamIndex() const
    {
        return streamIndex_;
    }

    void Start(bool paced)
    {
        if (running_.exchange(true)) {
            return;
        }
        paced_ = paced;
        stop_ = false;
        paused_ = false;
        worker_ = std::thread(&SubtitleDecoderCore::Run, this);
    }
    void Pause()
    {
        paused_ = true;
    }
    void Resume()
    {
        paused_ = false;
    }
    void Stop()
    {
        StopInternal();
    }

    // Leak-path zombie cleanup — same rationale as the video decoder: never
    // join anything on the JS thread; the worker exits on its own once the
    // queue is detached, and the possible last-reference session Shutdown
    // runs on a helper thread (Run holds its own session reference).
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
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFSubtitle", "demux detached (leak-path cleanup)");
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

    void CloseCodec()
    {
        if (sws_ != nullptr) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (dec_ != nullptr) {
            avcodec_free_context(&dec_);
        }
        // Shared demux: drop the session's routing reference to our queue
        // first, then release the claim/session; fmt_ is session-owned.
        if (queue_ != nullptr && session_ != nullptr) {
            session_->DetachQueue(queue_.get());
        }
        queue_ = nullptr;
        if (session_ != nullptr) {
            session_->CancelClaim();
            session_ = nullptr;
        }
        if (fmt_ != nullptr && !sharedFmt_) {
            avformat_close_input(&fmt_);
        }
        fmt_ = nullptr;
        sharedFmt_ = false;
        program_ = nullptr;
        streamIndex_ = -1;
        canvasWidth_ = 0;
        canvasHeight_ = 0;
        codecName_.clear();
        language_.clear();
        opened_ = false;
    }

    static void FreeMsg(SubMsg* msg)
    {
        if (msg->rgba != nullptr) {
            free(msg->rgba);
        }
        if (msg->pes != nullptr) {
            free(msg->pes);
        }
        delete msg;
    }

    // Non-blocking TSF emit. A full queue means the JS thread is not draining
    // (busy, or blocked in stop()/close() joining this very worker). Blocking
    // here would deadlock that join (6s thread-block kill). Subtitle bitmaps
    // are realtime display sets and drop on a full queue (the next clear event
    // restores a consistent state); end/error retry briefly and give up as
    // soon as stop_ is armed, so the join waits at most one retry interval.
    void Emit(SubMsg* msg)
    {
        if (tsfn_ == nullptr) {
            FreeMsg(msg);
            return;
        }
        napi_status st = napi_call_threadsafe_function(tsfn_, msg, napi_tsfn_nonblocking);
        if (st == napi_ok) {
            return;
        }
        // Only end/error deserve a bounded retry (see comment above). PES and
        // bitmap messages are realtime and drop silently on a full queue.
        bool retriable = msg->type == SUBMSG_END || msg->type == SUBMSG_ERROR;
        if (st == napi_queue_full && retriable) {
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
        if (retriable) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFSub",
                "drop tsfn msg type=%{public}d st=%{public}d (queue full or closing)",
                msg->type, static_cast<int>(st));
        }
        FreeMsg(msg);
    }

    void EmitEnd()
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_END;
        Emit(msg);
    }

    void EmitError(const std::string& err)
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_ERROR;
        msg->err = err;
        Emit(msg);
    }

    void EmitClear(double ptsMs)
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_SUBTITLE;
        msg->clear = true;
        msg->ptsMs = ptsMs;
        msg->canvasWidth = canvasWidth_;
        msg->canvasHeight = canvasHeight_;
        Emit(msg);
    }

    // Raw PES passthrough (DVB pump mode): forward the demuxed subtitle packet
    // (PES payload from data_identifier) untouched — the JS DVB chain
    // assembles display sets itself. This lets the JS subtitle pipeline reuse
    // the bytes the shared demux already downloaded instead of issuing a
    // second concurrent segment download (which the server throttles).
    void EmitPes(const AVPacket* pkt)
    {
        if (pkt->size <= 0) {
            return;
        }
        auto* msg = new SubMsg();
        msg->type = SUBMSG_PES;
        msg->pesPts = pkt->pts == AV_NOPTS_VALUE ? -1.0 : static_cast<double>(pkt->pts);
        msg->pesLen = static_cast<size_t>(pkt->size);
        msg->pes = static_cast<uint8_t*>(malloc(msg->pesLen));
        if (msg->pes != nullptr) {
            memcpy(msg->pes, pkt->data, msg->pesLen);
        } else {
            msg->pesLen = 0;
        }
        Emit(msg);
    }

    void EmitText(double ptsMs, double durationMs, const std::string& text)
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_SUBTITLE;
        msg->ptsMs = ptsMs;
        msg->durationMs = durationMs;
        msg->canvasWidth = canvasWidth_;
        msg->canvasHeight = canvasHeight_;
        msg->text = text;
        Emit(msg);
    }

    void EmitBitmap(double ptsMs, double durationMs, int x, int y, int w, int h, int regionCount,
        const uint8_t* rgba, size_t len)
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_SUBTITLE;
        msg->ptsMs = ptsMs;
        msg->durationMs = durationMs;
        msg->canvasWidth = canvasWidth_;
        msg->canvasHeight = canvasHeight_;
        msg->x = x;
        msg->y = y;
        msg->width = w;
        msg->height = h;
        msg->regionCount = regionCount;
        msg->rgbaLen = len;
        msg->rgba = static_cast<uint8_t*>(malloc(len));
        if (msg->rgba != nullptr) {
            memcpy(msg->rgba, rgba, len);
        } else {
            msg->rgbaLen = 0;
        }
        Emit(msg);
    }

    // Composites all bitmap regions of a display set into one RGBA strip.
    void DeliverSubtitle(const AVSubtitle& sub)
    {
        double baseMs =
            sub.pts == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(sub.pts) / (AV_TIME_BASE / 1000.0);
        double ptsMs = baseMs + static_cast<double>(sub.start_display_time);
        double endMs = baseMs + static_cast<double>(sub.end_display_time);
        double durationMs = endMs > ptsMs ? endMs - ptsMs : 0.0;

        if (dec_ != nullptr && dec_->width > 0 && dec_->height > 0) {
            canvasWidth_ = dec_->width;
            canvasHeight_ = dec_->height;
        }
        if (sub.num_rects <= 0) {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFSub", "subtitle clear pts=%{public}.0fms",
                ptsMs);
            EmitClear(ptsMs);
            return;
        }

        std::vector<RgbaRect> rects;
        std::string text;
        for (unsigned int i = 0; i < sub.num_rects; ++i) {
            const AVSubtitleRect* r = sub.rects[i];
            if (r == nullptr) {
                continue;
            }
            if (r->type == SUBTITLE_BITMAP && r->data[0] != nullptr && r->w > 0 && r->h > 0) {
                int x = r->x;
                int y = r->y;
                int w = r->w;
                int h = r->h;
                if (canvasWidth_ > 0) {
                    if (x < 0) {
                        w += x;
                        x = 0;
                    }
                    if (x + w > canvasWidth_) {
                        w = canvasWidth_ - x;
                    }
                }
                if (canvasHeight_ > 0) {
                    if (y < 0) {
                        h += y;
                        y = 0;
                    }
                    if (y + h > canvasHeight_) {
                        h = canvasHeight_ - y;
                    }
                }
                if (w <= 0 || h <= 0) {
                    continue;
                }
                RgbaRect rr;
                rr.x = x;
                rr.y = y;
                rr.w = w;
                rr.h = h;
                rr.rect = r;
                rects.push_back(rr);
            } else if (r->text != nullptr && r->text[0] != '\0') {
                if (!text.empty()) {
                    text += "\n";
                }
                text += r->text;
            } else if (r->ass != nullptr && r->ass[0] != '\0') {
                if (!text.empty()) {
                    text += "\n";
                }
                text += r->ass;
            }
        }
        if (rects.empty()) {
            if (!text.empty()) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFSub", "subtitle text len=%{public}zu",
                    text.size());
                EmitText(ptsMs, durationMs, text);
            } else {
                EmitClear(ptsMs);
            }
            return;
        }

        int bx0 = INT_MAX;
        int by0 = INT_MAX;
        int bx1 = 0;
        int by1 = 0;
        for (const RgbaRect& rr : rects) {
            bx0 = std::min(bx0, rr.x);
            by0 = std::min(by0, rr.y);
            bx1 = std::max(bx1, rr.x + rr.w);
            by1 = std::max(by1, rr.y + rr.h);
        }
        int bw = bx1 - bx0;
        int bh = by1 - by0;
        std::vector<uint8_t> canvas(static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4, 0);

        // palette-based (PAL8) regions, converted with swscale like ffplay does
        std::vector<uint32_t> defaultPalette;
        for (const RgbaRect& rr : rects) {
            const AVSubtitleRect* r = rr.rect;
            const uint8_t* pal = r->data[1];
            if (pal == nullptr) {
                if (defaultPalette.empty()) {
                    // A palette-less PAL8 region carries no color information;
                    // fall back to fully transparent instead of painting a
                    // solid white block over the video.
                    defaultPalette.assign(256, 0x00000000u);
                }
                pal = reinterpret_cast<const uint8_t*>(defaultPalette.data());
            }
            sws_ = sws_getCachedContext(sws_, r->w, r->h, AV_PIX_FMT_PAL8, r->w, r->h, AV_PIX_FMT_RGBA,
                SWS_POINT, nullptr, nullptr, nullptr);
            if (sws_ == nullptr) {
                EmitError("sws_getCachedContext failed");
                return;
            }
            const uint8_t* src[4] = { r->data[0], pal, nullptr, nullptr };
            int srcStride[4] = { r->linesize[0], 0, 0, 0 };
            uint8_t* dst[4] = { canvas.data() + (static_cast<size_t>(rr.y - by0) * static_cast<size_t>(bw) +
                                   static_cast<size_t>(rr.x - bx0)) * 4, nullptr, nullptr, nullptr };
            int dstStride[4] = { bw * 4, 0, 0, 0 };
            sws_scale(sws_, src, srcStride, 0, r->h, dst, dstStride);
        }

        // crop to the visible (non-transparent) area and drop fully transparent
        // display sets: DVB pages are frequently full-screen sized but only carry
        // a few lines of text, and there is no point shipping megabytes of alpha
        int cx0 = bw;
        int cy0 = bh;
        int cx1 = -1;
        int cy1 = -1;
        for (int yy = 0; yy < bh; ++yy) {
            const uint8_t* row = canvas.data() + static_cast<size_t>(yy) * static_cast<size_t>(bw) * 4;
            for (int xx = 0; xx < bw; ++xx) {
                if (row[static_cast<size_t>(xx) * 4 + 3] != 0) {
                    if (xx < cx0) {
                        cx0 = xx;
                    }
                    if (xx > cx1) {
                        cx1 = xx;
                    }
                    if (yy < cy0) {
                        cy0 = yy;
                    }
                    if (yy > cy1) {
                        cy1 = yy;
                    }
                }
            }
        }
        if (cx1 < 0) {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFSub",
                "subtitle page %{public}dx%{public}d is fully transparent", bw, bh);
            EmitClear(ptsMs);
            return;
        }
        int tw = cx1 - cx0 + 1;
        int th = cy1 - cy0 + 1;
        std::vector<uint8_t> tight(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);
        for (int yy = 0; yy < th; ++yy) {
            memcpy(tight.data() + static_cast<size_t>(yy) * static_cast<size_t>(tw) * 4,
                canvas.data() +
                    (static_cast<size_t>(yy + cy0) * static_cast<size_t>(bw) + static_cast<size_t>(cx0)) * 4,
                static_cast<size_t>(tw) * 4);
        }

        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFSub",
            "subtitle box=%{public}d,%{public}d %{public}dx%{public}d (page %{public}dx%{public}d) regions=%{public}zu canvas=%{public}dx%{public}d",
            bx0 + cx0, by0 + cy0, tw, th, bw, bh, rects.size(), canvasWidth_, canvasHeight_);
        EmitBitmap(ptsMs, durationMs, bx0 + cx0, by0 + cy0, tw, th, static_cast<int>(rects.size()),
            tight.data(), tight.size());
    }

    void EmitOverflow(uint64_t dropped)
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_OVERFLOW;
        msg->count = dropped;
        Emit(msg);
    }

    void EmitDiscontinuity(const std::string& reason)
    {
        auto* msg = new SubMsg();
        msg->type = SUBMSG_DISCONTINUITY;
        msg->err = reason;
        Emit(msg);
    }

    // Worker-side polling of the shared queue's overflow counter (F3): the
    // demux thread never calls back into a decoder; the worker notices a new
    // overflow episode on its next loop pass and emits. Also fires while
    // paused — a paused (or never-started) subtitle consumer is exactly when
    // the backlog overflows. A reconnect (A3) surfaces as a plain
    // discontinuity('reconnect'): the subtitle core carries no stats and no
    // dedicated reconnected event.
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
            EmitDiscontinuity("reconnect");
        }
    }

    // Unified packet source: the shared demux queue when attached, the private
    // av_read_frame otherwise. Returns av_read_frame-compatible status
    // (0 = ok, <0 = EOF or aborted).
    int ReadPacket(AVPacket* pkt)
    {
        if (queue_ != nullptr) {
            return queue_->Pop(pkt, stop_);
        }
        return av_read_frame(fmt_, pkt);
    }

    void Run()
    {
        // Hold the shared session for the whole worker lifetime: DetachDemux
        // (leak-path cleanup) may drop the core's reference from another
        // thread while this worker is still draining; fmt_ stays valid until
        // this local is destroyed.
        std::shared_ptr<DemuxSession> session = session_;
        AVPacket* pkt = av_packet_alloc();
        int64_t firstPts = AV_NOPTS_VALUE;
        auto startWall = std::chrono::steady_clock::now();
        bool pausing = false;
        auto pauseStart = std::chrono::steady_clock::now();

        while (!stop_) {
            PollQueueEvents();

            if (paused_) {
                if (!pausing) {
                    pausing = true;
                    pauseStart = std::chrono::steady_clock::now();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (pausing) {
                // Shift the pacing anchor by the paused span so subtitles do
                // not burst out to catch up with the wall clock after resume.
                startWall += std::chrono::steady_clock::now() - pauseStart;
                pausing = false;
            }
            int ret = ReadPacket(pkt);
            if (ret < 0) {
                EmitEnd();
                break;
            }
            if (pkt->stream_index == streamIndex_) {
                if (raw_) {
                    EmitPes(pkt);
                    av_packet_unref(pkt);
                    continue;
                }
                AVSubtitle sub;
                memset(&sub, 0, sizeof(sub));
                int got = 0;
                int used = avcodec_decode_subtitle2(dec_, &sub, &got, pkt);
                if (used >= 0 && got) {
                    if (paced_ && sub.pts != AV_NOPTS_VALUE) {
                        if (firstPts == AV_NOPTS_VALUE) {
                            firstPts = sub.pts;
                            startWall = std::chrono::steady_clock::now();
                        } else {
                            double targetMs =
                                static_cast<double>(sub.pts - firstPts) / (AV_TIME_BASE / 1000.0);
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - startWall).count();
                            int64_t waitMs = static_cast<int64_t>(targetMs) - static_cast<int64_t>(elapsed);
                            if (waitMs > 0 && waitMs < 2000) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                            }
                        }
                    }
                    DeliverSubtitle(sub);
                    avsubtitle_free(&sub);
                }
            }
            av_packet_unref(pkt);
        }
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
    AVProgram* program_ = nullptr;
    int streamIndex_ = -1;
    int32_t canvasWidth_ = 0;
    int32_t canvasHeight_ = 0;
    std::string codecName_;
    std::string language_;
    bool declared_ = false;
    bool opened_ = false;
    bool paced_ = true;
    // raw PES pump mode (see SetRawPump)
    bool raw_ = false;
    std::string lastError_;

    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> paused_{ false };

    napi_threadsafe_function tsfn_ = nullptr;
    napi_ref subtitleRef_ = nullptr;
    napi_ref endRef_ = nullptr;
    napi_ref errorRef_ = nullptr;
    napi_ref pesRef_ = nullptr;
    napi_ref overflowRef_ = nullptr;
    napi_ref discontinuityRef_ = nullptr;
    // worker-side overflow episode bookkeeping (see PollQueueEvents)
    uint64_t lastOverflowSeen_ = 0;
    uint64_t lastDroppedSeen_ = 0;
    uint64_t lastReconnectSeen_ = 0;
};

struct OpenContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    SubtitleDecoderCore* core = nullptr;
    std::string src;
    int streamIndex = -1;
    NetOptions netOpts;
    bool ok = false;
};

SubtitleDecoderCore* GetCore(napi_env env, napi_callback_info info)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVar, nullptr);
    SubtitleDecoderCore* core = nullptr;
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
    (void)status;
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
        napi_create_string_utf8(env, ctx->core->GetCodecName().c_str(), NAPI_AUTO_LENGTH, &v);
        napi_set_named_property(env, result, "codec", v);
        napi_create_string_utf8(env, ctx->core->GetLanguage().c_str(), NAPI_AUTO_LENGTH, &v);
        napi_set_named_property(env, result, "language", v);
        napi_create_int32(env, ctx->core->GetCanvasWidth(), &v);
        napi_set_named_property(env, result, "width", v);
        napi_create_int32(env, ctx->core->GetCanvasHeight(), &v);
        napi_set_named_property(env, result, "height", v);
        napi_create_int32(env, ctx->core->GetStreamIndex(), &v);
        napi_set_named_property(env, result, "streamIndex", v);
        napi_create_int64(env, ctx->core->GetDurationMs(), &v);
        napi_set_named_property(env, result, "durationMs", v);
        napi_get_boolean(env, true, &v);
        napi_set_named_property(env, result, "declared", v);
        napi_resolve_deferred(env, ctx->deferred, result);
    }
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

void TsfnDispatcher(napi_env env, napi_value jsCallback, void* context, void* data)
{
    (void)jsCallback;
    auto* core = static_cast<SubtitleDecoderCore*>(context);
    auto* msg = static_cast<SubMsg*>(data);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);

    if (msg->type == SUBMSG_SUBTITLE) {
        if (core != nullptr && core->GetSubtitleCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetSubtitleCallback(), &cb);
            if (cb != nullptr) {
                napi_value ev = nullptr;
                napi_create_object(env, &ev);
                napi_value v = nullptr;
                napi_get_boolean(env, msg->clear, &v);
                napi_set_named_property(env, ev, "clear", v);
                napi_create_double(env, msg->ptsMs, &v);
                napi_set_named_property(env, ev, "ptsMs", v);
                napi_create_double(env, msg->durationMs, &v);
                napi_set_named_property(env, ev, "durationMs", v);
                napi_create_int32(env, msg->canvasWidth, &v);
                napi_set_named_property(env, ev, "canvasWidth", v);
                napi_create_int32(env, msg->canvasHeight, &v);
                napi_set_named_property(env, ev, "canvasHeight", v);
                napi_create_int32(env, msg->x, &v);
                napi_set_named_property(env, ev, "x", v);
                napi_create_int32(env, msg->y, &v);
                napi_set_named_property(env, ev, "y", v);
                napi_create_int32(env, msg->width, &v);
                napi_set_named_property(env, ev, "width", v);
                napi_create_int32(env, msg->height, &v);
                napi_set_named_property(env, ev, "height", v);
                napi_create_int32(env, msg->regionCount, &v);
                napi_set_named_property(env, ev, "regionCount", v);
                napi_create_string_utf8(env, msg->text.c_str(), NAPI_AUTO_LENGTH, &v);
                napi_set_named_property(env, ev, "text", v);
                if (msg->rgba != nullptr && msg->rgbaLen > 0) {
                    void* abData = nullptr;
                    napi_value ab = nullptr;
                    napi_create_arraybuffer(env, msg->rgbaLen, &abData, &ab);
                    memcpy(abData, msg->rgba, msg->rgbaLen);
                    napi_set_named_property(env, ev, "rgba", ab);
                } else {
                    napi_value ab = nullptr;
                    napi_create_arraybuffer(env, 0, nullptr, &ab);
                    napi_set_named_property(env, ev, "rgba", ab);
                }
                napi_call_function(env, undefined, cb, 1, &ev, &undefined);
            }
        }
    } else if (msg->type == SUBMSG_END) {
        if (core != nullptr && core->GetEndCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetEndCallback(), &cb);
            if (cb != nullptr) {
                napi_call_function(env, undefined, cb, 0, nullptr, &undefined);
            }
        }
      } else if (msg->type == SUBMSG_ERROR) {
          if (core != nullptr && core->GetErrorCallback() != nullptr) {
              napi_value cb = nullptr;
              napi_get_reference_value(env, core->GetErrorCallback(), &cb);
              if (cb != nullptr) {
                  napi_value msgVal = nullptr;
                  napi_create_string_utf8(env, msg->err.c_str(), NAPI_AUTO_LENGTH, &msgVal);
                  napi_call_function(env, undefined, cb, 1, &msgVal, &undefined);
              }
          }
      } else if (msg->type == SUBMSG_PES) {
          if (core != nullptr && core->GetPesCallback() != nullptr &&
              msg->pes != nullptr && msg->pesLen > 0) {
              napi_value cb = nullptr;
              napi_get_reference_value(env, core->GetPesCallback(), &cb);
              if (cb != nullptr) {
                  void* abData = nullptr;
                  napi_value ab = nullptr;
                  if (napi_create_arraybuffer(env, msg->pesLen, &abData, &ab) == napi_ok) {
                      memcpy(abData, msg->pes, msg->pesLen);
                      napi_value ptsVal = nullptr;
                      napi_create_double(env, msg->pesPts, &ptsVal);
                      napi_value cbArgs[2] = { ab, ptsVal };
                      napi_call_function(env, undefined, cb, 2, cbArgs, &undefined);
                  }
              }
          }
      } else if (msg->type == SUBMSG_OVERFLOW) {
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
      } else if (msg->type == SUBMSG_DISCONTINUITY) {
          if (core != nullptr && core->GetDiscontinuityCallback() != nullptr) {
              napi_value cb = nullptr;
              napi_get_reference_value(env, core->GetDiscontinuityCallback(), &cb);
              if (cb != nullptr) {
                  napi_value ev = nullptr;
                  napi_create_object(env, &ev);
                  napi_value reason = nullptr;
                  napi_create_string_utf8(env, msg->err.c_str(), NAPI_AUTO_LENGTH, &reason);
                  napi_set_named_property(env, ev, "reason", reason);
                  napi_call_function(env, undefined, cb, 1, &ev, &undefined);
              }
          }
      }

      if (msg->rgba != nullptr) {
          free(msg->rgba);
      }
      if (msg->pes != nullptr) {
          free(msg->pes);
      }
      delete msg;
  }

void CoreFinalize(napi_env env, void* data, void* hint)
{
    (void)hint;
    auto* core = static_cast<SubtitleDecoderCore*>(data);
    // The JS object died without close(): unsubscribe the shared demux
    // session BEFORE the delete — a zombie queue keeps receiving routed
    // packets until the destructor runs (endless high-water drop storm, and
    // the demux thread can sit blocked on the queue's full mark while the
    // queue object is being destroyed).
    core->DetachDemux();
    core->ReleaseTsfn();
    napi_ref sub = core->GetSubtitleCallback();
    napi_ref end = core->GetEndCallback();
    napi_ref err = core->GetErrorCallback();
    napi_ref pes = core->GetPesCallback();
    napi_ref overflow = core->GetOverflowCallback();
    napi_ref discont = core->GetDiscontinuityCallback();
    if (sub != nullptr) {
        napi_delete_reference(env, sub);
    }
    if (end != nullptr) {
        napi_delete_reference(env, end);
    }
    if (err != nullptr) {
        napi_delete_reference(env, err);
    }
    if (pes != nullptr) {
        napi_delete_reference(env, pes);
    }
    if (overflow != nullptr) {
        napi_delete_reference(env, overflow);
    }
    if (discont != nullptr) {
        napi_delete_reference(env, discont);
    }
    delete core;
}

} // namespace

napi_value FFSubtitleDecoderConstructor(napi_env env, napi_callback_info info)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVar, nullptr);
    auto* core = new SubtitleDecoderCore();
    napi_wrap(env, thisVar, core, CoreFinalize, nullptr, nullptr);
    return thisVar;
}

napi_value FFSubtitleDecoderOpen(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = { nullptr, nullptr, nullptr };
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, &argc, args, &thisVar, nullptr);
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFSubtitleDecoder is not constructed");
        return nullptr;
    }
    if (argc < 1) {
        napi_throw_type_error(env, nullptr,
            "open(src: string, streamIndex?: number, options?: object) requires src");
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
            "options must be { userAgent?, headers?, allowedExtensions?, liveStartIndex?, analyzeMs?, program?, httpPersistent?, reconnect?, variantIndex?, maxBandwidth?, targetLatencyMs?, autoReconnect?, reconnectMaxRetries?, dvrSeek? }");
        return nullptr;
    }

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_subtitle_open", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, thisVar, resourceName, OpenExecute, OpenComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value FFSubtitleDecoderOn(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    SubtitleDecoderCore* core = GetCore(env, info);
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
    if (event == "subtitle") {
        slot = core->SubtitleSlot();
    } else if (event == "end") {
        slot = core->EndSlot();
    } else if (event == "error") {
        slot = core->ErrorSlot();
    } else if (event == "pes") {
        slot = core->PesSlot();
    } else if (event == "overflow") {
        slot = core->OverflowSlot();
    } else if (event == "discontinuity") {
        slot = core->DiscontinuitySlot();
    } else {
        napi_throw_type_error(env, nullptr,
            "unsupported event, expect 'subtitle' | 'end' | 'error' | 'pes' | 'overflow' | 'discontinuity'");
        return nullptr;
    }
    if (*slot != nullptr) {
        napi_delete_reference(env, *slot);
    }
    napi_create_reference(env, args[1], 1, slot);
    return nullptr;
}

napi_value FFSubtitleDecoderStart(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFSubtitleDecoder is not constructed");
        return nullptr;
    }
    if (!core->IsOpened()) {
        napi_throw_error(env, nullptr, "FFSubtitleDecoder is not opened");
        return nullptr;
    }
    bool paced = true;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_bool(env, args[0], &paced);
    }
    if (core->GetTsfn() == nullptr) {
        napi_value resourceName = nullptr;
        napi_create_string_utf8(env, "ffmpeg_subtitle_tsfn", NAPI_AUTO_LENGTH, &resourceName);
        napi_threadsafe_function tsfn = nullptr;
        napi_create_threadsafe_function(env, nullptr, nullptr, resourceName, 32, 2,
            nullptr, nullptr, core, TsfnDispatcher, &tsfn);
        core->SetTsfn(tsfn);
    }
    core->Start(paced);
    return nullptr;
}

napi_value FFSubtitleDecoderPause(napi_env env, napi_callback_info info)
{
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Pause();
    }
    return nullptr;
}

napi_value FFSubtitleDecoderResume(napi_env env, napi_callback_info info)
{
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Resume();
    }
    return nullptr;
}

napi_value FFSubtitleDecoderStop(napi_env env, napi_callback_info info)
{
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Stop();
    }
    return nullptr;
}

napi_value FFSubtitleDecoderClose(napi_env env, napi_callback_info info)
{
    SubtitleDecoderCore* core = GetCore(env, info);
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

napi_value FFSubtitleDecoderDetach(napi_env env, napi_callback_info info)
{
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->DetachDemux();
    }
    return nullptr;
}

napi_value FFSubtitleDecoderSetRawPump(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    SubtitleDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFSubtitleDecoder is not constructed");
        return nullptr;
    }
    bool raw = false;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_bool(env, args[0], &raw);
    }
    core->SetRawPump(raw);
    return nullptr;
}

} // namespace ffmpeg
