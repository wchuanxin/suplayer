#include "audio_decoder.h"
#include "net_options.h"
#include "demux_session.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ffmpeg {
namespace {

enum MsgType {
    MSG_PCM = 0,
    MSG_END = 1,
    MSG_ERROR = 2,
    MSG_SWITCHED = 3,
};

struct DecoderMsg {
    int type = MSG_PCM;
    uint8_t* data = nullptr;
    size_t len = 0;
    double ptsMs = 0.0;
    int64_t durationMs = 0;
    int32_t sampleRate = 0;
    int32_t channels = 2;
    uint64_t channelMask = 0;
    std::string err;
    std::string codec;
};

// One parallel-decoding audio track: own decoder + resampler, so that
// switching the active track needs no re-open and no re-buffering.
struct AudioTrack {
    int index = -1;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    std::string codec;
    int64_t decodedFrames = 0;
    bool eof = false;
};

std::string ErrToStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

int InterruptOnStop(void* opaque)
{
    auto* flag = static_cast<std::atomic<bool>*>(opaque);
    return flag->load(std::memory_order_relaxed) ? 1 : 0;
}

class AudioDecoderCore {
public:
    AudioDecoderCore() = default;
    ~AudioDecoderCore()
    {
        StopInternal();
        CloseCodec();
    }

    // callback ref slots, used on JS thread
    napi_ref* PcmSlot()
    {
        return &pcmRef_;
    }
    napi_ref* EndSlot()
    {
        return &endRef_;
    }
    napi_ref* ErrorSlot()
    {
        return &errorRef_;
    }
    napi_ref* SwitchedSlot()
    {
        return &switchedRef_;
    }
    napi_ref GetPcmCallback() const
    {
        return pcmRef_;
    }
    napi_ref GetEndCallback() const
    {
        return endRef_;
    }
    napi_ref GetErrorCallback() const
    {
        return errorRef_;
    }
    napi_ref GetSwitchedCallback() const
    {
        return switchedRef_;
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

    bool Open(const std::string& src, int streamIndex, const NetOptions& netOpts)
    {
        CloseCodec();
        audioAllTracks_ = netOpts.audioAllTracks;
        stop_ = false;
        AVProgram* program = nullptr;
        if (netOpts.sharedDemux) {
            // 共享 demux 会话：一条连接服务同源的全部解码器；open/流分析/
            // program 锁定/startTimeMs seek 均由会话统一完成
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
            fmt_->interrupt_callback.callback = InterruptOnStop;
            fmt_->interrupt_callback.opaque = &stop_;
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
            // startTimeMs 定位起播（续播/seek）：跳到目标前最近可达位置，解码循环
            // 丢弃目标前帧实现精确起点；不可 seek（直播等）时回退从头播放
            dropBeforeMs_ = 0.0;
            if (netOpts.startTimeMs > 0) {
                int64_t targetUs = static_cast<int64_t>(netOpts.startTimeMs) * 1000;
                if (avformat_seek_file(fmt_, -1, INT64_MIN, targetUs, INT64_MAX, 0) >= 0) {
                    dropBeforeMs_ = static_cast<double>(netOpts.startTimeMs);
                } else {
                    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFAudioSW",
                        "seek to %{public}lldms failed, starting from head",
                        static_cast<long long>(netOpts.startTimeMs));
                }
            }
        }
        if (streamIndex < 0) {
            if (program != nullptr) {
                streamIndex = FindStreamInProgram(fmt_, program, AVMEDIA_TYPE_AUDIO);
            } else {
                streamIndex = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            }
            if (streamIndex < 0) {
                lastError_ = "no audio stream found";
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
        AudioTrack* initial = OpenTrackDecoder(streamIndex);
        if (initial == nullptr) {
            CloseCodec();
            return false;
        }
        // shared output format: every track resamples to the initial track's
        // rate and channel layout so the app-side renderer stays valid across
        // hot switches
        outRate_ = initial->dec->sample_rate > 0 ? initial->dec->sample_rate : 44100;
        BuildOutLayout(initial->dec->ch_layout);
        if (!ConfigureTrackSwr(initial, outRate_)) {
            FreeTrack(initial);
            CloseCodec();
            return false;
        }
        tracks_.push_back(initial);
        streamIndex_ = streamIndex;
        // activate every other allowed audio track for parallel receive.
        // audioTracks:'first' skips this — only the selected track decodes
        // (lower CPU/power); switchTrack opens the target track lazily.
        if (audioAllTracks_) {
            for (unsigned int i = 0; i < fmt_->nb_streams; ++i) {
                int si = static_cast<int>(i);
                if (si == streamIndex || !StreamInProgram(program, si)) {
                    continue;
                }
                if (fmt_->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                    continue;
                }
                AudioTrack* t = OpenTrackDecoder(si);
                if (t == nullptr) {
                    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFAudioSW",
                        "parallel track #%{public}d open failed, skipped", si);
                    continue;
                }
                if (!ConfigureTrackSwr(t, outRate_)) {
                    FreeTrack(t);
                    continue;
                }
                tracks_.push_back(t);
            }
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW",
            "audio tracks=%{public}zu active=%{public}d parallel=%{public}s outRate=%{public}d "
            "outCh=%{public}d outMask=%{public}llu",
            tracks_.size(), streamIndex_, audioAllTracks_ ? "all" : "first",
            outRate_, outChannels_, static_cast<unsigned long long>(outMask_));
        if (sharedFmt_ && session_ != nullptr) {
            // 订阅 program 内全部音频流（未锁定 program 时为全部音频流）：
            // 未开轨的包由 Run 循环丢弃，语义与私有路径一致
            std::vector<int> audioStreams;
            for (unsigned int i = 0; i < fmt_->nb_streams; ++i) {
                int si = static_cast<int>(i);
                if (fmt_->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                    continue;
                }
                if (!StreamInProgram(program, si)) {
                    continue;
                }
                audioStreams.push_back(si);
            }
            if (!audioStreams.empty()) {
                queue_ = session_->Subscribe(audioStreams);
                // Live audio: same catch-up overflow policy as video, but
                // resume at the next packet (audio frames are independently
                // decodable). This also covers a consumer that never pops
                // (renderer start hang while backgrounded): blocking
                // backpressure there would deadlock the whole shared session.
                queue_->SetLiveDrop(netOpts.live, false);
            }
        }
        program_ = program;
        opened_ = true;
        return true;
    }

    AudioTrack* OpenTrackDecoder(int streamIndex)
    {
        if (fmt_ == nullptr || static_cast<unsigned int>(streamIndex) >= fmt_->nb_streams) {
            lastError_ = "stream index out of range";
            return nullptr;
        }
        AVStream* stream = fmt_->streams[streamIndex];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            lastError_ = "stream is not audio";
            return nullptr;
        }
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            lastError_ = "unsupported audio codec";
            return nullptr;
        }
        AVCodecContext* dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, stream->codecpar);
        int ret = avcodec_open2(dec, codec, nullptr);
        if (ret < 0) {
            lastError_ = "avcodec_open2 failed: " + ErrToStr(ret);
            avcodec_free_context(&dec);
            return nullptr;
        }
        auto* t = new AudioTrack();
        t->index = streamIndex;
        t->dec = dec;
        t->codec = codec->name != nullptr ? codec->name : "";
        return t;
    }

    // Locks the shared output channel layout to the initial track's layout.
    // Mono..8-channel native layouts pass through; unspecified layouts fall
    // back to the default layout of that channel count; anything exotic
    // (custom maps, ambisonics, >8 channels) is downmixed to stereo.
    void BuildOutLayout(const AVChannelLayout& src)
    {
        outChannels_ = 2;
        outMask_ = AV_CH_LAYOUT_STEREO;
        const int nb = src.nb_channels;
        if (nb < 1 || nb > 8) {
            return;
        }
        if (src.order == AV_CHANNEL_ORDER_NATIVE) {
            outChannels_ = nb;
            outMask_ = src.u.mask;
            return;
        }
        if (src.order == AV_CHANNEL_ORDER_UNSPEC) {
            AVChannelLayout def {};
            av_channel_layout_default(&def, nb);
            if (def.nb_channels == nb && def.order == AV_CHANNEL_ORDER_NATIVE) {
                outChannels_ = nb;
                outMask_ = def.u.mask;
            }
        }
    }

    bool ConfigureTrackSwr(AudioTrack* t, int outRate)
    {
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MASK(outChannels_, outMask_);
        int ret = swr_alloc_set_opts2(&t->swr,
            &outLayout, AV_SAMPLE_FMT_FLT, outRate,
            &t->dec->ch_layout, t->dec->sample_fmt, t->dec->sample_rate,
            0, nullptr);
        if (ret < 0 || t->swr == nullptr || swr_init(t->swr) < 0) {
            lastError_ = "swr init failed";
            swr_free(&t->swr);
            return false;
        }
        return true;
    }

    // Varispeed reconfig: build the new SwrContext FIRST and only swap it in
    // on success — a failed in-place reconfig would free t->swr and leave the
    // worker crashing on a null context mid-stream.
    void ApplyRateMilli(int rateMilli)
    {
        // Direction: the renderer consumes samples at a FIXED outRate_. Fewer
        // output samples per decoded frame = shorter wall playback = FASTER.
        // A slowdown (rateMilli < 1000) therefore needs MORE samples per frame:
        // swr out rate = outRate_ / rate. (outRate_ * rate inverted the effect
        // and drove live A/V skew the wrong way.)
        if (rateMilli < 500) {
            rateMilli = 500;
        }
        const int effectiveRate = static_cast<int>(
            llround(static_cast<double>(outRate_) * 1000.0 / rateMilli));
        for (AudioTrack* t : tracks_) {
            if (t->dec == nullptr) {
                continue;
            }
            SwrContext* swr = nullptr;
            AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MASK(outChannels_, outMask_);
            int ret = swr_alloc_set_opts2(&swr,
                &outLayout, AV_SAMPLE_FMT_FLT, effectiveRate,
                &t->dec->ch_layout, t->dec->sample_fmt, t->dec->sample_rate,
                0, nullptr);
            if (ret < 0 || swr == nullptr || swr_init(swr) < 0) {
                swr_free(&swr);
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFAudioSW",
                    "rate reconfig failed (%{public}d milli), keep previous", rateMilli);
                return;
            }
            swr_free(&t->swr);
            t->swr = swr;
        }
        activeRateMilli_ = rateMilli;
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW",
            "audio rate %{public}d/1000 (swr out=%{public}dHz, renderer stays %{public}dHz)",
            rateMilli, effectiveRate, outRate_);
    }

    void FreeTrack(AudioTrack* t)
    {
        if (t == nullptr) {
            return;
        }
        if (t->swr != nullptr) {
            swr_free(&t->swr);
        }
        if (t->dec != nullptr) {
            avcodec_free_context(&t->dec);
        }
        delete t;
    }

    AudioTrack* FindTrack(int index)
    {
        for (AudioTrack* t : tracks_) {
            if (t->index == index) {
                return t;
            }
        }
        return nullptr;
    }

    const AudioTrack* FindTrack(int index) const
    {
        for (const AudioTrack* t : tracks_) {
            if (t->index == index) {
                return t;
            }
        }
        return nullptr;
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
    int GetSampleRate() const
    {
        return outRate_;
    }
    // output is interleaved FLT at the locked channel layout (source-faithful)
    int GetChannels() const
    {
        return outChannels_;
    }
    // FFmpeg-native channel mask of the locked output layout (0x3 = stereo)
    uint64_t GetChannelMask() const
    {
        return outMask_;
    }
    std::string GetCodecName() const
    {
        const AudioTrack* t = FindTrack(streamIndex_);
        return t != nullptr ? t->codec : "";
    }

    void Start(bool paced)
    {
        if (running_.exchange(true)) {
            return;
        }
        paced_ = paced;
        stop_ = false;
        paused_ = false;
        worker_ = std::thread(&AudioDecoderCore::Run, this);
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

    void RequestSwitch(int streamIndex)
    {
        pendingSwitch_ = streamIndex;
    }

    // Varispeed (live A/V sync micro-correction): the renderer keeps consuming
    // at a FIXED outRate_, so playback speed = outRate_ / swr-out-rate. A rate
    // below 1.0 (slowdown) therefore RAISES the swr output rate: more samples
    // per decoded frame => each packet takes longer to play, no discontinuity
    // and no dropped content. The worker loop owns all SwrContexts, so only
    // the request is stored here.
    void SetRate(double rate)
    {
        if (!(rate >= 0.98 && rate <= 1.02)) {
            rate = rate < 0.98 ? 0.98 : 1.02;
        }
        pendingRateMilli_.store(static_cast<int>(llround(rate * 1000.0)), std::memory_order_relaxed);
    }

    // A/V resync: drop the shared-queue backlog and resume at the next routed
    // packet (audio frames are independently decodable). The still-running
    // worker re-anchors its pacing on the forward pts jump (waitMs >= 1000),
    // and the JS side flushes its PCM write queue on the same jump.
    void FlushBacklog()
    {
        if (queue_ == nullptr) {
            return; // private connection: no dropable backlog
        }
        queue_->DropBacklogToKeyframe(); // skipToKey off for audio: drops all
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
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW", "demux detached (leak-path cleanup)");
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
        for (AudioTrack* t : tracks_) {
            FreeTrack(t);
        }
        tracks_.clear();
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
        opened_ = false;
    }

    static void FreeMsg(DecoderMsg* msg)
    {
        if (msg->data != nullptr) {
            free(msg->data);
        }
        delete msg;
    }

    // Non-blocking TSF emit. A full queue means the JS thread is not draining
    // (busy, or blocked in stop()/close() joining this very worker). Blocking
    // here would deadlock that join (6s thread-block kill): the JS thread can
    // never drain the queue while it waits inside pthread_join. Realtime PCM
    // drops on a full queue (the renderer resyncs on pts anyway); critical
    // events (end/error/switched) retry briefly and give up as soon as stop_
    // is armed, so the join waits at most one retry interval.
    void Emit(DecoderMsg* msg)
    {
        if (tsfn_ == nullptr) {
            FreeMsg(msg);
            return;
        }
        napi_status st = napi_call_threadsafe_function(tsfn_, msg, napi_tsfn_nonblocking);
        if (st == napi_ok) {
            return;
        }
        if (st == napi_queue_full && msg->type != MSG_PCM) {
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
        if (msg->type != MSG_PCM) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFAudioSW",
                "drop tsfn msg type=%{public}d st=%{public}d (queue full or closing)",
                msg->type, static_cast<int>(st));
        }
        FreeMsg(msg);
    }

    void EmitPcm(const uint8_t* data, size_t len, double ptsMs)
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_PCM;
        msg->len = len;
        msg->ptsMs = ptsMs;
        msg->data = static_cast<uint8_t*>(malloc(len));
        memcpy(msg->data, data, len);
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

    void EmitSwitched(const AudioTrack* t)
    {
        auto* msg = new DecoderMsg();
        msg->type = MSG_SWITCHED;
        msg->durationMs = GetDurationMs();
        msg->sampleRate = outRate_;
        msg->channels = outChannels_;
        msg->channelMask = outMask_;
        msg->codec = t->codec;
        Emit(msg);
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
        AVFrame* frame = av_frame_alloc();
        bool eofSent = false;
        const int maxSamples = 8192;
        uint8_t* outBuf = static_cast<uint8_t*>(malloc(maxSamples * outChannels_ * sizeof(float)));
        int64_t firstPts = AV_NOPTS_VALUE;
    int dropGuard = 0;
        auto startWall = std::chrono::steady_clock::now();
        bool pausing = false;
        auto pauseStart = std::chrono::steady_clock::now();
        AudioTrack* active = FindTrack(streamIndex_);

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
                // Shift the pacing anchor by the paused span so packets do not
                // burst out to catch up with the wall clock after resume.
                startWall += std::chrono::steady_clock::now() - pauseStart;
                pausing = false;
            }

            int sw = pendingSwitch_.exchange(-1);
            if (sw >= 0 && sw != streamIndex_) {
                AudioTrack* t = FindTrack(sw);
                if (!StreamInProgram(program_, sw)) {
                    EmitError("switchTrack failed: stream index " + std::to_string(sw) +
                        " does not belong to the locked program");
                } else if (t == nullptr) {
                    // track was not activated at open (open failed then); retry lazily
                    t = OpenTrackDecoder(sw);
                    // lazy-open must inherit the ACTIVE varispeed rate; same
                    // direction rule as ApplyRateMilli (out rate = outRate_/rate)
                    const int trackRate = activeRateMilli_ == 1000 ? outRate_ : static_cast<int>(
                        llround(static_cast<double>(outRate_) * 1000.0 / activeRateMilli_));
                    if (t != nullptr && ConfigureTrackSwr(t, trackRate)) {
                        tracks_.push_back(t);
                    } else {
                        FreeTrack(t);
                        t = nullptr;
                        EmitError("switchTrack failed: " + lastError_);
                    }
                }
                if (t != nullptr) {
                    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW",
                        "hot switch from=%{public}d to=%{public}d activeFrames=%{public}lld newFrames=%{public}lld",
                        streamIndex_, sw, active != nullptr ? static_cast<long long>(active->decodedFrames) : 0LL,
                        static_cast<long long>(t->decodedFrames));
                    streamIndex_ = sw;
                    active = t;
                    firstPts = AV_NOPTS_VALUE; // re-anchor pacing to now
                    EmitSwitched(t);
                }
            }

            // varispeed: apply a pending rate change before decoding further
            // frames so the whole reconfig stays on the worker thread
            int rq = pendingRateMilli_.load(std::memory_order_relaxed);
            if (rq != activeRateMilli_) {
                ApplyRateMilli(rq);
            }

            int pendingRate = pendingRateMilli_.load(std::memory_order_relaxed);
            if (pendingRate != activeRateMilli_) {
                ApplyRateMilli(pendingRate);
            }

            // drain every parallel decoder; only the active track is converted and emitted
            bool activeEof = false;
            bool decodeError = false;
            for (AudioTrack* t : tracks_) {
                if (t->eof) {
                    if (t == active) {
                        activeEof = true;
                    }
                    continue;
                }
                while (!stop_) {
                    int ret = avcodec_receive_frame(t->dec, frame);
                    if (ret == AVERROR(EAGAIN)) {
                        break;
                    }
                    if (ret == AVERROR_EOF) {
                        t->eof = true;
                        if (t == active) {
                            activeEof = true;
                        }
                        break;
                    }
                    if (ret < 0) {
                        EmitError("avcodec_receive_frame failed: " + ErrToStr(ret));
                        decodeError = true;
                        break;
                    }
                    t->decodedFrames++;
                    if (t == active) {
                        double ptsMs = 0.0;
                        AVStream* stream = fmt_->streams[t->index];
                        int64_t pts = frame->best_effort_timestamp;
                        if (pts == AV_NOPTS_VALUE) {
                            pts = frame->pts;
                        }
                        if (pts != AV_NOPTS_VALUE) {
                            ptsMs = static_cast<double>(pts) * av_q2d(stream->time_base) * 1000.0;
                        }
                        // startTimeMs precise start: skip resample/emit for
                        // pre-target frames. Guard cap protects streams with
                        // broken timestamps (all NOPTS → ptsMs 0) from
                        // dropping forever.
                        if (dropBeforeMs_ > 0.0) {
                            if (pts == AV_NOPTS_VALUE || ptsMs < dropBeforeMs_) {
                                av_frame_unref(frame);
                                if (++dropGuard > 3600) {
                                    dropBeforeMs_ = 0.0;
                                    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFAudioSW",
                                        "pre-target drop guard hit, emitting from current frame");
                                }
                                continue;
                            }
                            dropBeforeMs_ = 0.0;
                        }
                        int outSamples = swr_convert(t->swr, &outBuf, maxSamples,
                            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                        if (outSamples < 0) {
                            EmitError("swr_convert failed: " + ErrToStr(outSamples));
                            decodeError = true;
                            av_frame_unref(frame);
                            break;
                        }
                        if (paced_) {
                            if (firstPts == AV_NOPTS_VALUE) {
                                firstPts = pts;
                                startWall = std::chrono::steady_clock::now();
                            } else if (pts != AV_NOPTS_VALUE) {
                                double targetMs =
                                    static_cast<double>(pts - firstPts) * av_q2d(stream->time_base) * 1000.0;
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - startWall).count();
                                int64_t waitMs = static_cast<int64_t>(targetMs) - static_cast<int64_t>(elapsed);
                                if (waitMs >= 1000) {
                                    // Forward pts jump (live skip / edge reconnect):
                                    // re-anchor the pacing instead of bursting.
                                    // The old anchor would drop the sleep cap and
                                    // decode at full speed, flooding the JS write
                                    // queue whose drop valve discards the excess
                                    // anyway — an audible chunk would be lost.
                                    firstPts = pts;
                                    startWall = std::chrono::steady_clock::now();
                                } else if (waitMs > 0) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                                }
                            }
                        }
                        if (outSamples > 0) {
                            EmitPcm(outBuf, static_cast<size_t>(outSamples) * outChannels_ * sizeof(float), ptsMs);
                        }
                    }
                    av_frame_unref(frame);
                }
                if (decodeError || stop_) {
                    break;
                }
            }
            if (decodeError) {
                break;
            }
            if (activeEof) {
                EmitEnd();
                break;
            }

            int ret = ReadPacket(pkt);
            if (ret < 0) {
                if (!eofSent) {
                    for (AudioTrack* t : tracks_) {
                        avcodec_send_packet(t->dec, nullptr);
                    }
                    eofSent = true;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            AudioTrack* t = FindTrack(pkt->stream_index);
            if (t != nullptr && !t->eof) {
                avcodec_send_packet(t->dec, pkt);
            }
            av_packet_unref(pkt);
        }

        free(outBuf);
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }

    AVFormatContext* fmt_ = nullptr;
    // Shared demux (B1): fmt_ is then BORROWED from the session and must not
    // be closed here; packets come from queue_ instead of av_read_frame.
    std::shared_ptr<DemuxSession> session_;
    std::shared_ptr<PacketQueue> queue_;
    bool sharedFmt_ = false;
    std::vector<AudioTrack*> tracks_; // all parallel audio decoders
    AVProgram* program_ = nullptr;
    int streamIndex_ = -1; // active (emitted) track index
    int outRate_ = 44100;  // shared output rate across all tracks
    int outChannels_ = 2;  // shared output channel count across all tracks
    uint64_t outMask_ = AV_CH_LAYOUT_STEREO; // shared output channel mask
    bool opened_ = false;
    std::string lastError_;
    // startTimeMs precise start: active-track frames with pts below this (ms)
    // are decoded but neither resampled nor emitted; cleared once the first
    // target frame passes
    double dropBeforeMs_ = 0.0;

    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> paused_{ false };
    std::atomic<int> pendingSwitch_{ -1 };
    // varispeed request (milli-rate, 1000 = 1.0x); worker applies it by
    // reconfiguring every track swr with out rate = outRate_ * rate
    std::atomic<int> pendingRateMilli_{ 1000 };
    int activeRateMilli_ = 1000; // worker-only mirror
    bool paced_ = false;
    // audioTracks option: decode all tracks in parallel (true) or only the
    // selected one, opening others lazily on switchTrack (false)
    bool audioAllTracks_ = true;

    napi_threadsafe_function tsfn_ = nullptr;
    napi_ref pcmRef_ = nullptr;
    napi_ref endRef_ = nullptr;
    napi_ref errorRef_ = nullptr;
    napi_ref switchedRef_ = nullptr;
};

struct OpenContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    AudioDecoderCore* core = nullptr;
    std::string src;
    int streamIndex = -1;
    NetOptions netOpts;
    bool ok = false;
};

AudioDecoderCore* GetCore(napi_env env, napi_callback_info info)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVar, nullptr);
    AudioDecoderCore* core = nullptr;
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
        napi_create_int64(env, ctx->core->GetDurationMs(), &v);
        napi_set_named_property(env, result, "durationMs", v);
        napi_create_int32(env, ctx->core->GetSampleRate(), &v);
        napi_set_named_property(env, result, "sampleRate", v);
        napi_create_int32(env, ctx->core->GetChannels(), &v);
        napi_set_named_property(env, result, "channels", v);
        napi_create_int64(env, static_cast<int64_t>(ctx->core->GetChannelMask()), &v);
        napi_set_named_property(env, result, "channelLayout", v);
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
    auto* core = static_cast<AudioDecoderCore*>(context);
    auto* msg = static_cast<DecoderMsg*>(data);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);

    if (msg->type == MSG_PCM) {
        if (core != nullptr && core->GetPcmCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetPcmCallback(), &cb);
            if (cb != nullptr) {
                void* abData = nullptr;
                napi_value ab = nullptr;
                napi_create_arraybuffer(env, msg->len, &abData, &ab);
                memcpy(abData, msg->data, msg->len);
                napi_value packet = nullptr;
                napi_create_object(env, &packet);
                napi_value pts = nullptr;
                napi_create_double(env, msg->ptsMs, &pts);
                napi_set_named_property(env, packet, "data", ab);
                napi_set_named_property(env, packet, "ptsMs", pts);
                napi_call_function(env, undefined, cb, 1, &packet, &undefined);
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
    } else if (msg->type == MSG_SWITCHED) {
        if (core != nullptr && core->GetSwitchedCallback() != nullptr) {
            napi_value cb = nullptr;
            napi_get_reference_value(env, core->GetSwitchedCallback(), &cb);
            if (cb != nullptr) {
                napi_value info = nullptr;
                napi_create_object(env, &info);
                napi_value v = nullptr;
                napi_create_int64(env, msg->durationMs, &v);
                napi_set_named_property(env, info, "durationMs", v);
                napi_create_int32(env, msg->sampleRate, &v);
                napi_set_named_property(env, info, "sampleRate", v);
                napi_create_int32(env, msg->channels, &v);
                napi_set_named_property(env, info, "channels", v);
                napi_create_int64(env, static_cast<int64_t>(msg->channelMask), &v);
                napi_set_named_property(env, info, "channelLayout", v);
                napi_create_string_utf8(env, msg->codec.c_str(), NAPI_AUTO_LENGTH, &v);
                napi_set_named_property(env, info, "codec", v);
                napi_call_function(env, undefined, cb, 1, &info, &undefined);
            }
        }
    }

    if (msg->data != nullptr) {
        free(msg->data);
    }
    delete msg;
}

void CoreFinalize(napi_env env, void* data, void* hint)
{
    (void)hint;
    auto* core = static_cast<AudioDecoderCore*>(data);
    core->ReleaseTsfn();
    napi_ref pcm = core->GetPcmCallback();
    napi_ref end = core->GetEndCallback();
    napi_ref err = core->GetErrorCallback();
    napi_ref sw = core->GetSwitchedCallback();
    if (pcm != nullptr) {
        napi_delete_reference(env, pcm);
    }
    if (end != nullptr) {
        napi_delete_reference(env, end);
    }
    if (err != nullptr) {
        napi_delete_reference(env, err);
    }
    if (sw != nullptr) {
        napi_delete_reference(env, sw);
    }
    delete core;
}

} // namespace

napi_value FFAudioDecoderConstructor(napi_env env, napi_callback_info info)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVar, nullptr);
    auto* core = new AudioDecoderCore();
    napi_wrap(env, thisVar, core, CoreFinalize, nullptr, nullptr);
    return thisVar;
}

napi_value FFAudioDecoderOpen(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = { nullptr, nullptr, nullptr };
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, &argc, args, &thisVar, nullptr);
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
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
    napi_create_string_utf8(env, "ffmpeg_audio_open", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, thisVar, resourceName, OpenExecute, OpenComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value FFAudioDecoderOn(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    AudioDecoderCore* core = GetCore(env, info);
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
    if (event == "pcm") {
        slot = core->PcmSlot();
    } else if (event == "end") {
        slot = core->EndSlot();
    } else if (event == "error") {
        slot = core->ErrorSlot();
    } else if (event == "switched") {
        slot = core->SwitchedSlot();
    } else {
        napi_throw_type_error(env, nullptr, "unsupported event, expect 'pcm' | 'end' | 'error' | 'switched'");
        return nullptr;
    }
    if (*slot != nullptr) {
        napi_delete_reference(env, *slot);
    }
    napi_create_reference(env, args[1], 1, slot);
    return nullptr;
}

napi_value FFAudioDecoderStart(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
        return nullptr;
    }
    if (!core->IsOpened()) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not opened");
        return nullptr;
    }
    bool paced = true;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_bool(env, args[0], &paced);
    }
    if (core->GetTsfn() == nullptr) {
        napi_value resourceName = nullptr;
        napi_create_string_utf8(env, "ffmpeg_audio_tsfn", NAPI_AUTO_LENGTH, &resourceName);
        napi_threadsafe_function tsfn = nullptr;
        // PCM queue cap 96 (~3 s at 32 ms/packet): sub-second JS-thread
        // stalls (system GC / vsync timeouts) must not hit the native drop
        // path — the app-side write queue valve sheds the excess on recovery.
        napi_create_threadsafe_function(env, nullptr, nullptr, resourceName, 96, 2,
            nullptr, nullptr, core, TsfnDispatcher, &tsfn);
        core->SetTsfn(tsfn);
    }
    core->Start(paced);
    return nullptr;
}

napi_value FFAudioDecoderSwitchTrack(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
        return nullptr;
    }
    if (!core->IsOpened()) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not opened");
        return nullptr;
    }
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "switchTrack(streamIndex: number) requires streamIndex");
        return nullptr;
    }
    int32_t streamIndex = -1;
    napi_get_value_int32(env, args[0], &streamIndex);
    if (streamIndex < 0) {
        napi_throw_type_error(env, nullptr, "streamIndex must be >= 0");
        return nullptr;
    }
    core->RequestSwitch(streamIndex);
    return nullptr;
}

napi_value FFAudioDecoderPause(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Pause();
    }
    return nullptr;
}

napi_value FFAudioDecoderResume(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Resume();
    }
    return nullptr;
}

napi_value FFAudioDecoderStop(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->Stop();
    }
    return nullptr;
}

napi_value FFAudioDecoderClose(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
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

napi_value FFAudioDecoderFlushBacklog(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->FlushBacklog();
    }
    return nullptr;
}

napi_value FFAudioDecoderSetRate(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
        return nullptr;
    }
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "setRate(rate: number) requires a rate argument");
        return nullptr;
    }
    double rate = 1.0;
    napi_get_value_double(env, args[0], &rate);
    core->SetRate(rate);
    return nullptr;
}

napi_value FFAudioDecoderDetach(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core != nullptr) {
        core->DetachDemux();
    }
    return nullptr;
}

} // namespace ffmpeg
