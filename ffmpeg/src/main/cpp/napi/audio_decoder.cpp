#include "audio_decoder.h"
#include "net_options.h"
#include "demux_session.h"
#include "hls_playlist.h"
#include "stream_io.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    MSG_OVERFLOW = 4,
    MSG_DISCONTINUITY = 5,
    MSG_RECONNECTED = 6,
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
    uint64_t count = 0; // MSG_OVERFLOW: packets dropped in the episode
    std::string err;    // MSG_ERROR: message; MSG_DISCONTINUITY: reason
    std::string codec;
    int64_t num = INT64_MIN; // MSG_DISCONTINUITY: effective seek target (ms)
};

// Completion handshake for seekTo(): the NAPI async work arms the request and
// blocks on the condition variable; the worker applies it and notifies.
struct SeekAck {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string err;
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

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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
    napi_ref* OverflowSlot()
    {
        return &overflowRef_;
    }
    napi_ref* DiscontinuitySlot()
    {
        return &discontinuityRef_;
    }
    napi_ref* ReconnectedSlot()
    {
        return &reconnectedRef_;
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
    napi_ref GetOverflowCallback() const
    {
        return overflowRef_;
    }
    napi_ref GetDiscontinuityCallback() const
    {
        return discontinuityRef_;
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

    bool Open(const std::string& src, int streamIndex, const NetOptions& netOpts)
    {
        CloseCodec();
        audioAllTracks_ = netOpts.audioAllTracks;
        stop_ = false;
        AVProgram* program = nullptr;
        if (netOpts.sharedDemux) {
            // 鍏变韩 demux 浼氳瘽锛氫竴鏉¤繛鎺ユ湇鍔″悓婧愮殑鍏ㄩ儴瑙ｇ爜鍣紱open/娴佸垎鏋?
            // program 閿佸畾/startTimeMs seek 鍧囩敱浼氳瘽缁熶竴瀹屾垚
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
            // A2/F1: variant/latency resolution (infallible 鈥?falls back to
            // the original URL) before the private open.
            const SourceResolution res = ResolveSource(src, netOpts, &InterruptOnStop, &stop_);
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
            // startTimeMs 瀹氫綅璧锋挱锛堢画鎾?seek锛夛細璺冲埌鐩爣鍓嶆渶杩戝彲杈句綅缃紝瑙ｇ爜寰幆
            // 涓㈠純鐩爣鍓嶅抚瀹炵幇绮剧‘璧风偣锛涗笉鍙?seek锛堢洿鎾瓑锛夋椂鍥為€€浠庡ご鎾斁
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
        // startTimeMs 鐨勬祦绾?seek 宸插湪涓婃柟绉佹湁璺緞 / 浼氳瘽鍒涘缓鏃跺畬鎴?
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
        // audioTracks:'first' skips this 鈥?only the selected track decodes
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
            // 璁㈤槄 program 鍐呭叏閮ㄩ煶棰戞祦锛堟湭閿佸畾 program 鏃朵负鍏ㄩ儴闊抽娴侊級锛?
            // 鏈紑杞ㄧ殑鍖呯敱 Run 寰幆涓㈠純锛岃涔変笌绉佹湁璺緞涓€鑷?
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
                // Stats stream: the initial track (first audio stream of the
                // program) 鈥?the one actually decoded at startup.
                queue_->SetStatsStream(audioStreams[0]);
                // Live audio: same catch-up overflow policy as video, but
                // resume at the next packet (audio frames are independently
                // decodable). This also covers a consumer that never pops
                // (renderer start hang while backgrounded): blocking
                // backpressure there would deadlock the whole shared session.
                BackpressurePolicy policy;
                policy.liveDrop = netOpts.live;
                queue_->SetPolicy(policy);
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
    // on success 鈥?a failed in-place reconfig would free t->swr and leave the
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

    // ---- User-level speed (atempo) ------------------------------------
    // Pipeline: decoded frame -> track swr (FLT @ outRate_) -> abuffer ->
    // atempo -> abuffersink -> emit. Emitted chunk pts uses a sample-counter
    // content anchor: N output samples at speed S cover N*S/outRate_ seconds
    // of content, independent of atempo's internal buffering and its own
    // pts bookkeeping (WSOLA window delays chunks but the anchor stays exact).
    bool InitSpeedFilter(double speed)
    {
        ResetSpeedFilter();
        speedGraph_ = avfilter_graph_alloc();
        if (speedGraph_ == nullptr) {
            lastError_ = "atempo graph alloc failed";
            return false;
        }
        const AVFilter* src = avfilter_get_by_name("abuffer");
        const AVFilter* tempo = avfilter_get_by_name("atempo");
        const AVFilter* sink = avfilter_get_by_name("abuffersink");
        if (src == nullptr || tempo == nullptr || sink == nullptr) {
            ResetSpeedFilter();
            lastError_ = "atempo filters unavailable";
            return false;
        }
        char args[256];
        snprintf(args, sizeof(args),
            "time_base=1/%d:sample_rate=%d:sample_fmt=flt:channel_layout=0x%llx",
            outRate_, outRate_, static_cast<unsigned long long>(outMask_));
        if (avfilter_graph_create_filter(&speedSrc_, src, "speed_src", args, nullptr, speedGraph_) < 0 ||
            avfilter_graph_create_filter(&speedSink_, sink, "speed_sink", nullptr, nullptr, speedGraph_) < 0) {
            ResetSpeedFilter();
            lastError_ = "atempo graph create failed";
            return false;
        }
        char tempoArgs[32];
        snprintf(tempoArgs, sizeof(tempoArgs), "tempo=%.6f", speed);
        AVFilterContext* tempoCtx = nullptr;
        if (avfilter_graph_create_filter(&tempoCtx, tempo, "atempo", tempoArgs, nullptr, speedGraph_) < 0 ||
            avfilter_link(speedSrc_, 0, tempoCtx, 0) < 0 ||
            avfilter_link(tempoCtx, 0, speedSink_, 0) < 0 ||
            avfilter_graph_config(speedGraph_, nullptr) < 0) {
            ResetSpeedFilter();
            lastError_ = "atempo graph config failed";
            return false;
        }
        speedInSamples_ = 0;
        speedAnchored_ = false;
        return true;
    }

    void ResetSpeedFilter()
    {
        if (speedGraph_ != nullptr) {
            avfilter_graph_free(&speedGraph_); // frees src/sink contexts too
        }
        speedSrc_ = nullptr;
        speedSink_ = nullptr;
        speedInSamples_ = 0;
        speedAnchored_ = false;
    }

    // Push one swr output chunk through atempo and emit every drained chunk.
    // The content pts of the first output sample anchors to the fed chunk's
    // pts; afterwards the anchor advances by the sample-count rule.
    void FeedSpeedFilter(const uint8_t* data, int samples, double ptsMs, double speed)
    {
        if (speedGraph_ == nullptr && !InitSpeedFilter(speed)) {
            // atempo unavailable: degrade to pass-through rather than
            // dropping audio
            EmitPcm(data, static_cast<size_t>(samples) * outChannels_ * sizeof(float), ptsMs);
            return;
        }
        if (!speedAnchored_) {
            speedPtsMs_ = ptsMs;
            speedAnchored_ = true;
        }
        AVFrame* in = av_frame_alloc();
        if (in == nullptr) {
            return;
        }
        in->format = AV_SAMPLE_FMT_FLT;
        av_channel_layout_from_mask(&in->ch_layout, outMask_);
        in->sample_rate = outRate_;
        in->nb_samples = samples;
        if (av_frame_get_buffer(in, 0) < 0) {
            av_frame_free(&in);
            EmitError("atempo input buffer alloc failed");
            return;
        }
        memcpy(in->data[0], data, static_cast<size_t>(samples) * outChannels_ * sizeof(float));
        in->pts = speedInSamples_;
        speedInSamples_ += samples;
        if (av_buffersrc_add_frame_flags(speedSrc_, in, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
            EmitError("atempo feed failed");
        }
        av_frame_free(&in);
        DrainSpeedFilter(speed);
    }

    void DrainSpeedFilter(double speed)
    {
        AVFrame* out = av_frame_alloc();
        if (out == nullptr) {
            return;
        }
        while (true) {
            const int ret = av_buffersink_get_frame(speedSink_, out);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                EmitError("atempo drain failed: " + ErrToStr(ret));
                break;
            }
            const int n = out->nb_samples;
            if (n > 0) {
                EmitPcm(out->data[0], static_cast<size_t>(n) * outChannels_ * sizeof(float), speedPtsMs_);
                speedPtsMs_ += static_cast<double>(n) * 1000.0 * speed / static_cast<double>(outRate_);
            }
            av_frame_unref(out);
        }
        av_frame_free(&out);
    }

    // EOF flush: signal end-of-stream to atempo and drain its internal
    // window so the tail is not lost.
    void FlushSpeedFilter(double speed)
    {
        if (speedGraph_ == nullptr) {
            return;
        }
        if (speedSrc_ != nullptr) {
            if (av_buffersrc_add_frame(speedSrc_, nullptr) < 0) {
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFAudioSW", "atempo eof signal failed");
            }
        }
        DrainSpeedFilter(speed);
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

    // ---- Runtime VOD seek (private + shared 鏂规鈶? ----------------------
    // Arms a seek; the worker applies it at the top of the decode loop
    // (avformat_seek_file -> flush all track decoders -> pacing re-anchor ->
    // pre-target drop filter) and completes `ack`. Shared connection: the
    // session's demux thread applies the seek and notifies the queue; the
    // worker re-anchors on the epoch advance (鏂规鈶?.
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
    // claimed the ack this is a no-op (the caller keeps it alive until the
    // worker completes it).
    void CancelSeekAck(SeekAck* ack)
    {
        std::lock_guard<std::mutex> lock(seekMtx_);
        if (seekAck_ == ack) {
            seekAck_ = nullptr;
            pendingSeekMs_.store(INT64_MIN, std::memory_order_release);
        }
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

    // User-level playback speed (atempo, 0.5x-2.0x, pitch preserved).
    // Orthogonal to SetRate micro-varispeed: actual output rate is
    // speed x rate. Only the request is stored here; the worker rebuilds
    // its atempo graph on the change (draining buffered samples first).
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
        EmitDiscontinuity("resync");
    }

    // Leak-path zombie cleanup 鈥?same rationale as the video decoder: never
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
            CancelPendingSeek("decoder stopped");
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        // The worker exited without applying an armed seek: complete it now
        // so a blocked seekTo() promise resolves.
        CancelPendingSeek("decoder stopped");
    }

    void CancelPendingSeek(const char* reason)
    {
        SeekAck* seekAck = nullptr;
        {
            std::lock_guard<std::mutex> lock(seekMtx_);
            seekAck = seekAck_;
            seekAck_ = nullptr;
            pendingSeekMs_.store(INT64_MIN, std::memory_order_release);
        }
        if (seekAck != nullptr) {
            FinishSeek(seekAck, false, reason);
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
    // (epoch > armedSeekEpoch_). The caller re-anchors (track decoder
    // flush, pacing, pre-target drop filter) and completes the ack.
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
            AVIOContext* ioPb = fmt_->pb;
            avformat_close_input(&fmt_);
            ReleaseStreamIO(ioPb);
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
        // OVERFLOW/DISCONTINUITY never retry: FlushBacklog emits the resync
        // notice from the JS thread, and a full-queue retry loop there would
        // block the JS thread (worker-emitted events still retry below).
        const bool critical = msg->type == MSG_END || msg->type == MSG_ERROR || msg->type == MSG_SWITCHED;
        if (st == napi_queue_full && critical) {
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
    // demux thread never calls back into a decoder; the worker notices a new
    // overflow episode on its next loop pass and emits. Also fires while
    // paused 鈥?a paused live consumer is exactly when the backlog overflows.
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
    // (0 = ok, <0 = EOF or aborted).
    int ReadPacket(AVPacket* pkt)
    {
        int ret = -1;
        if (queue_ != nullptr) {
            ret = queue_->Pop(pkt, stop_);
        } else {
            ret = av_read_frame(fmt_, pkt);
        }
        if (ret == 0) {
            AccountReadBytes(static_cast<uint64_t>(pkt->size));
        }
        return ret;
    }

    // Byte counter + measured bitrate over a 5s window (D4). Worker-thread
    // only 鈥?unlike video there is no JS-thread peek on this path.
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
            PollQueueEvents();

            // 鏂规鈶?shared-session seek: the demux thread moved the shared
            // connection and dropped our backlog; re-anchor every track
            // decoder, pacing and the pre-target drop filter to the notified
            // target and complete the pending ack. The 'seek' discontinuity
            // makes the JS side flush its PCM write queue.
            {
                int64_t sharedTargetMs = 0;
                SeekAck* sharedAck = nullptr;
                bool sharedOk = false;
                if (TakeSharedSeek(sharedTargetMs, &sharedAck, sharedOk)) {
                    if (sharedOk) {
                        for (AudioTrack* t : tracks_) {
                            avcodec_flush_buffers(t->dec);
                            t->eof = false;
                        }
                        dropBeforeMs_ = static_cast<double>(sharedTargetMs);
                        dropGuard = 0;
                        firstPts = AV_NOPTS_VALUE;
                        startWall = std::chrono::steady_clock::now();
                        eofSent = false;
                        ResetSpeedFilter(); // atempo window holds pre-seek audio
                        EmitDiscontinuity("seek", sharedTargetMs);
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW",
                            "seek applied (shared) target=%{public}lldms",
                            static_cast<long long>(sharedTargetMs));
                        FinishSeek(sharedAck, true, "");
                    } else if (sharedAck != nullptr) {
                        FinishSeek(sharedAck, false, "avformat_seek_file failed on shared session");
                    }
                    continue;
                }
            }

            // Runtime VOD seek (private connection): flush every track
            // decoder, seek the connection to the target keyframe, re-anchor
            // pacing and re-arm the pre-target drop filter.
            if (pendingSeekMs_.load(std::memory_order_acquire) != INT64_MIN) {
                SeekAck* seekAck = nullptr;
                const int64_t seekMs = TakePendingSeek(&seekAck);
                if (seekMs != INT64_MIN) {
                    const int sret = avformat_seek_file(fmt_, -1, INT64_MIN, seekMs * 1000, INT64_MAX, 0);
                    if (sret >= 0) {
                        for (AudioTrack* t : tracks_) {
                            avcodec_flush_buffers(t->dec);
                            t->eof = false;
                        }
                        dropBeforeMs_ = static_cast<double>(seekMs);
                        dropGuard = 0;
                        firstPts = AV_NOPTS_VALUE;
                        startWall = std::chrono::steady_clock::now();
                        eofSent = false;
                        ResetSpeedFilter(); // atempo window holds pre-seek audio
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW",
                            "seek applied target=%{public}lldms", static_cast<long long>(seekMs));
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
                    speedAnchored_ = false;    // re-anchor atempo content pts to the new track
                    EmitSwitched(t);
                }
            }

            // varispeed: apply a pending rate change before decoding further
            // frames so the whole reconfig stays on the worker thread
            int rq = pendingRateMilli_.load(std::memory_order_relaxed);
            if (rq != activeRateMilli_) {
                ApplyRateMilli(rq);
            }

            // user-level speed: drain the old atempo graph (buffered samples
            // keep the old timeline), then rebuild for the new speed; 1.0x
            // needs no graph at all (direct pass-through)
            if (pendingSpeed_.load(std::memory_order_relaxed) != activeSpeed_) {
                if (speedGraph_ != nullptr) {
                    FlushSpeedFilter(activeSpeed_);
                    ResetSpeedFilter();
                }
                activeSpeed_ = pendingSpeed_.load(std::memory_order_relaxed);
                if (activeSpeed_ != 1.0 && !InitSpeedFilter(activeSpeed_)) {
                    activeSpeed_ = 1.0;
                    EmitError("atempo init failed: " + lastError_ + ", falling back to 1.0x");
                }
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFAudioSW",
                    "audio speed set to %{public}.2fx (atempo)", activeSpeed_);
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
                        // broken timestamps (all NOPTS 鈫?ptsMs 0) from
                        // dropping forever.
                        if (dropBeforeMs_ > 0.0) {
                            if (pts == AV_NOPTS_VALUE || ptsMs < dropBeforeMs_) {
                                av_frame_unref(frame);
                                droppedFrames_.fetch_add(1, std::memory_order_relaxed);
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
                                // speed divides the wall target: content
                                // delta D plays in D/speed wall seconds
                                double targetMs =
                                    static_cast<double>(pts - firstPts) * av_q2d(stream->time_base) * 1000.0 /
                                    (activeSpeed_ > 0.0 ? activeSpeed_ : 1.0);
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - startWall).count();
                                int64_t waitMs = static_cast<int64_t>(targetMs) - static_cast<int64_t>(elapsed);
                                if (waitMs >= 1000) {
                                    // Forward pts jump (live skip / edge reconnect):
                                    // re-anchor the pacing instead of bursting.
                                    // The old anchor would drop the sleep cap and
                                    // decode at full speed, flooding the JS write
                                    // queue whose drop valve discards the excess
                                    // anyway 鈥?an audible chunk would be lost.
                                    firstPts = pts;
                                    startWall = std::chrono::steady_clock::now();
                                } else if (waitMs > 0) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                                }
                            }
                        }
                        if (outSamples > 0) {
                            decodedFrames_.fetch_add(1, std::memory_order_relaxed);
                            if (activeSpeed_ != 1.0) {
                                // atempo path: pitch-preserved compression with
                                // sample-counter content-pts anchoring
                                FeedSpeedFilter(outBuf, outSamples, ptsMs, activeSpeed_);
                            } else {
                                EmitPcm(outBuf, static_cast<size_t>(outSamples) * outChannels_ * sizeof(float), ptsMs);
                            }
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
                FlushSpeedFilter(activeSpeed_); // drain atempo's tail window
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
        ResetSpeedFilter();
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
    // varispeed request (milli-rate, 1000 = 1.0x); worker applies it by
    // reconfiguring every track swr with out rate = outRate_ * rate
    std::atomic<int> pendingRateMilli_{ 1000 };
    int activeRateMilli_ = 1000; // worker-only mirror
    // ---- User-level speed (atempo, 0.5x-2.0x, pitch preserved) ----
    // NAPI stores the request; the worker owns the filter graph, the input
    // sample counter and the content-pts anchor of the output chunks
    std::atomic<double> pendingSpeed_{ 1.0 };
    double activeSpeed_ = 1.0; // worker-only mirror
    AVFilterGraph* speedGraph_ = nullptr;
    AVFilterContext* speedSrc_ = nullptr;
    AVFilterContext* speedSink_ = nullptr;
    double speedPtsMs_ = 0.0;    // content pts of the next atempo output sample
    bool speedAnchored_ = false; // false: anchor at the next fed chunk
    int64_t speedInSamples_ = 0; // input sample counter (abuffer pts, 1/rate)
    bool paced_ = false;
    // audioTracks option: decode all tracks in parallel (true) or only the
    // selected one, opening others lazily on switchTrack (false)
    bool audioAllTracks_ = true;

    // ---- Buffer / playback stats (D4/B2) ------------------------------
    std::atomic<uint64_t> readBytes_{ 0 };
    std::atomic<uint64_t> measuredBitrateBps_{ 0 };
    std::atomic<uint64_t> decodedFrames_{ 0 };
    std::atomic<uint64_t> droppedFrames_{ 0 };
    std::atomic<uint64_t> discontinuityCount_{ 0 };
    int64_t rateWindowStartMs_ = 0;
    uint64_t rateWindowBytes_ = 0;
    uint64_t lastOverflowSeen_ = 0;
    uint64_t lastDroppedSeen_ = 0;
    uint64_t lastReconnectSeen_ = 0;

    napi_threadsafe_function tsfn_ = nullptr;
    napi_ref pcmRef_ = nullptr;
    napi_ref endRef_ = nullptr;
    napi_ref errorRef_ = nullptr;
    napi_ref switchedRef_ = nullptr;
    napi_ref overflowRef_ = nullptr;
    napi_ref discontinuityRef_ = nullptr;
    napi_ref reconnectedRef_ = nullptr;
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
    // The JS object died without close(): unsubscribe the shared demux
    // session BEFORE the delete 鈥?a zombie queue keeps receiving routed
    // packets until the destructor runs (endless high-water drop storm, and
    // the demux thread can sit blocked on the queue's full mark while the
    // queue object is being destroyed).
    core->DetachDemux();
    core->ReleaseTsfn();
    napi_ref pcm = core->GetPcmCallback();
    napi_ref end = core->GetEndCallback();
    napi_ref err = core->GetErrorCallback();
    napi_ref sw = core->GetSwitchedCallback();
    napi_ref overflow = core->GetOverflowCallback();
    napi_ref discont = core->GetDiscontinuityCallback();
    napi_ref reconnected = core->GetReconnectedCallback();
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
    if (overflow != nullptr) {
        napi_delete_reference(env, overflow);
    }
    if (discont != nullptr) {
        napi_delete_reference(env, discont);
    }
    if (reconnected != nullptr) {
        napi_delete_reference(env, reconnected);
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
            "options must be { userAgent?, headers?, allowedExtensions?, liveStartIndex?, analyzeMs?, hw?, program?, httpPersistent?, reconnect?, variantIndex?, maxBandwidth?, targetLatencyMs?, autoReconnect?, reconnectMaxRetries?, dvrSeek?, rtspTransport?, bufferSize?, rwTimeoutMs? }");
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
    } else if (event == "overflow") {
        slot = core->OverflowSlot();
    } else if (event == "discontinuity") {
        slot = core->DiscontinuitySlot();
    } else if (event == "reconnected") {
        slot = core->ReconnectedSlot();
    } else {
        napi_throw_type_error(env, nullptr,
            "unsupported event, expect 'pcm' | 'end' | 'error' | 'switched' | 'overflow' | 'discontinuity' | 'reconnected'");
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
        // path 鈥?the app-side write queue valve sheds the excess on recovery.
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
        // growing +N per rebuild cycle 鈥?a wedged demux session was never
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

napi_value FFAudioDecoderSetSpeed(napi_env env, napi_callback_info info)
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
        napi_throw_type_error(env, nullptr, "setSpeed(speed: number) requires a speed argument");
        return nullptr;
    }
    double speed = 1.0;
    napi_get_value_double(env, args[0], &speed);
    core->SetSpeed(speed);
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

napi_value FFAudioDecoderGetBufferStats(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
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
    return result;
}

napi_value FFAudioDecoderGetStats(napi_env env, napi_callback_info info)
{
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
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

struct AudioSeekContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    AudioDecoderCore* core = nullptr;
    int64_t targetMs = 0;
    SeekAck ack;
    bool armed = false;
    bool timedOut = false;
};

void AudioSeekExecute(napi_env env, void* data)
{
    auto* ctx = static_cast<AudioSeekContext*>(data);
    std::string err;
    if (!ctx->core->RequestSeek(ctx->targetMs, &ctx->ack, err)) {
        ctx->ack.err = err;
        return;
    }
    ctx->armed = true;
    std::unique_lock<std::mutex> lock(ctx->ack.mtx);
    if (!ctx->ack.cv.wait_for(lock, std::chrono::seconds(15), [&ctx] { return ctx->ack.done; })) {
        ctx->timedOut = true;
        ctx->core->CancelSeekAck(&ctx->ack);
    }
}

void AudioSeekComplete(napi_env env, napi_status status, void* data)
{
    auto* ctx = static_cast<AudioSeekContext*>(data);
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

} // namespace

napi_value FFAudioDecoderSeekTo(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, info, &argc, args, &thisVar, nullptr);
    AudioDecoderCore* core = GetCore(env, info);
    if (core == nullptr) {
        napi_throw_error(env, nullptr, "FFAudioDecoder is not constructed");
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
    auto* ctx = new AudioSeekContext();
    ctx->core = core;
    ctx->targetMs = static_cast<int64_t>(target);
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_audio_seek", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, thisVar, resourceName, AudioSeekExecute, AudioSeekComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

} // namespace ffmpeg
