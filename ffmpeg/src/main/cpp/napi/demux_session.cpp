#include "demux_session.h"
#include "stream_io.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <hilog/log.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace ffmpeg {
namespace {

// Live read-stall watchdog: a live connection that has not delivered a
// single packet for this long is dead (server closed silently / network
// black hole) 鈥?av_read_frame would block forever because the interrupt
// callback only aborts on stopping_. Fire the interrupt so the read fails,
// the demux thread exits (demuxAlive_ -> false) and the next open builds a
// fresh session; the app's recovery machinery can then actually recover.
// Well under the threshold in normal operation: packets arrive every
// ~20-40 ms, segment-boundary gaps (playlist reload + next segment
// download) stay within a few seconds even on poor networks.
constexpr int64_t LIVE_READ_STALL_MS = 30000;

// Live throughput (slow-starve) watchdog: a server-throttled connection
// trickles packets (the 30s no-packet watchdog never fires) while playback
// crawls at a fraction of realtime. Judge by the video stream's media-time
// progress per wall-clock window 鈥?immune to prestart bursts (ratio >> 1) and
// to pts jumps (per-packet duration, not pts deltas).
constexpr int64_t LIVE_STARVE_WINDOW_MS = 10000;
constexpr double LIVE_STARVE_RATIO = 0.15;   // < 15% of realtime = starving
constexpr double LIVE_STARVE_HEALTHY = 0.5;  // >= 50% of realtime = healthy
constexpr int64_t LIVE_STARVE_MS = 15000;    // sustained starve before abort

// ABR (1.16): variant switching cadence. Downgrades arm once the starve
// streak outlives ABR_STARVE_SWITCH_MS (two consecutive watchdog windows)
// — ahead of LIVE_STARVE_MS so a lower rung gets the chance to rescue the
// session before the abort kills it. Upgrades are health probes: they need
// a sustained >= ABR_UP_HEALTHY streak and fire at most once per dwell
// window; a starve-driven downgrade doubles the dwell (penalizing a failed
// probe) up to ABR_UP_DWELL_MAX_MS. ABR_UP_STABLE_MS of sustained health
// resets the penalty.
constexpr int64_t ABR_STARVE_SWITCH_MS = 8000;
constexpr double ABR_UP_HEALTHY = 0.9;      // window counts as upgrade-healthy
constexpr int64_t ABR_UP_STABLE_MS = 60000; // sustained health resets the dwell
constexpr int64_t ABR_UP_DWELL_MS = 30000;  // initial up-probe dwell
constexpr int64_t ABR_UP_DWELL_MAX_MS = 300000; // dwell cap after failed probes

// Fast-landing threshold for shared seeks (鏂规鈶?: the seek positions the
// demuxer at the keyframe <= target. When that keyframe is further back than
// this, decoding the whole pre-target intra-GOP span would stall the landing
// for many seconds (hw decode is a few times realtime at best), so the
// consumers re-anchor on the keyframe itself (MX-Player-style fast seek) 鈥?
// the effective target is published through NotifySeek and carried in the
// 'seek' discontinuity event. Smaller gaps keep exact landing: waiting out
// the decode costs less than the position error of landing early.
constexpr int64_t FAST_SEEK_GAP_MS = 12000;

// GOP-adaptive stream analysis for live raw-transport sources (raw TS / FLV
// / direct live URLs, not HLS playlists). avformat_find_stream_info() exits
// as soon as every stream carries codec parameters — for MPEG-TS video that
// is the first in-band SPS/IDR — so max_analyze_duration is an upper bound,
// not a wait: short-GOP streams pay nothing extra under a larger cap.
// A live raw-transport join lands mid-GOP; when the app's analyzeMs is
// shorter than the stream GOP (IPTV measures 4-6s) the window closes before
// the next IDR and the open carries video width/height = 0 — the app's
// surface configure then fails and the pipeline degrades into the
// hw→sw→wedge chain. Repeated find_stream_info() calls cannot extend the
// window: codec_info_duration persists on the stream and the replayed
// packet_buffer re-accrues the already-analyzed media time against the
// raised budget (the second call re-processes the buffered packets and hits
// the new cap almost without reading new data). Instead raise the cap once,
// before the single call, and let the built-in early exit provide the
// adaptivity. probesize is raised in step so a high-bitrate source does not
// hit the 5MB default byte cap before the media-time cap (10s @ 6Mbps
// already exceeds it).
constexpr int64_t LIVE_RAW_ANALYZE_CAP_MS = 10000;
constexpr int64_t LIVE_RAW_PROBESIZE = 12 * 1024 * 1024;

bool RaiseLiveAnalyzeWindow(AVFormatContext* fmt, const NetOptions& opts, bool isHlsPlaylist)
{
    // Gates: live raw-transport only — HLS segments start on a keyframe so
    // the app's tighter analyzeMs is enough there (and keeps zapping fast);
    // an explicit analyzeMs > 0 signals the caller tuned the window;
    // startTimeMs would discard the analysis window on the post-open seek.
    if (fmt == nullptr || !opts.live || opts.analyzeMs <= 0 || isHlsPlaylist || opts.startTimeMs > 0) {
        return false;
    }
    if (static_cast<int64_t>(opts.analyzeMs) >= LIVE_RAW_ANALYZE_CAP_MS) {
        return false;
    }
    fmt->max_analyze_duration = LIVE_RAW_ANALYZE_CAP_MS * 1000;
    fmt->probesize = LIVE_RAW_PROBESIZE;
    return true;
}

std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::weak_ptr<DemuxSession>>& Registry()
{
    static std::unordered_map<std::string, std::weak_ptr<DemuxSession>> registry;
    return registry;
}

int64_t NowSteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ============================ DemuxSession ============================

std::string DemuxSession::BuildKey(const std::string& src, const NetOptions& opts)
{
    // Only demux-relevant options participate: two decoders whose demux
    // behavior differs must not share a connection. Decoder-local options
    // (hwMode / audioAllTracks / frameEventMs) and startTimeMs are excluded:
    // attachers start at the session's current demux position and compensate
    // with their own pre-target drop filter.
    std::string key = src;
    key += "|ua=" + opts.userAgent;
    key += "|hdr=" + opts.headers;
    key += "|ext=" + opts.allowedExtensions;
    key += "|lsi=" + std::to_string(opts.liveStartIndex);
    key += "|ana=" + std::to_string(opts.analyzeMs);
    key += "|prog=" + std::to_string(opts.program);
    key += "|pers=" + std::to_string(opts.httpPersistent ? 1 : 0);
    key += "|rc=" + std::to_string(opts.reconnect ? 1 : 0);
    key += "|vi=" + std::to_string(opts.variantIndex);
    key += "|bw=" + std::to_string(opts.maxBandwidth);
    key += "|abr=" + std::to_string(opts.abr ? 1 : 0);
    key += "|lat=" + std::to_string(opts.targetLatencyMs);
    key += "|arc=" + std::to_string(opts.autoReconnect);
    key += "|rcm=" + std::to_string(opts.reconnectMaxRetries);
    // Protocol options change the connection itself (transport/buffer/timeout)
    // and must not be shared across sessions with different values.
    key += "|rtsp=" + opts.rtspTransport;
    key += "|bufsz=" + std::to_string(opts.bufferSize);
    key += "|rwto=" + std::to_string(opts.rwTimeoutMs);
    return key;
}

std::shared_ptr<DemuxSession> DemuxSession::LookupFresh(const std::string& key)
{
    // Caller holds the registry mutex. A session is reusable only while it
    // still has subscribers or outstanding claims; one lingering only in a
    // transient probe reference is stale (its demux position belongs to a
    // previous playback) and must be replaced.
    auto it = Registry().find(key);
    if (it == Registry().end()) {
        return nullptr;
    }
    auto session = it->second.lock();
    if (session == nullptr) {
        Registry().erase(it);
        return nullptr;
    }
    if (!session->HasSubscribersOrClaims()) {
        return nullptr;
    }
    // A session whose demux thread already exited (EOF / read error) is a
    // corpse: its subscribers are draining their remaining packets, but a
    // new open must never attach to a session nobody feeds.
    if (!session->demuxAlive_.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    return session;
}

std::shared_ptr<DemuxSession> DemuxSession::GetOrCreate(const std::string& src, const NetOptions& opts,
    std::string& err)
{
    const std::string key = BuildKey(src, opts);
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto existing = LookupFresh(key);
        if (existing != nullptr) {
            existing->AddClaim(); // claim consumed by the caller's Subscribe
            return existing;
        }
    }
    // Miss: open privately WITHOUT the registry lock so concurrent first
    // openers stay parallel (startup latency) and the loser simply attaches.
    auto fresh = std::shared_ptr<DemuxSession>(new DemuxSession());
    if (!fresh->Init(src, opts, err)) {
        // The parallel fresh open failed 鈥?e.g. the server enforces a single
        // playlist connection per token/uid and closed ours while the
        // concurrent winner stayed connected. By now that winner is usually
        // already registered: attach to it instead of failing the whole
        // decoder open (observed as video-open-failure -> audio-only
        // playback with a black screen).
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto existing = LookupFresh(key);
        if (existing != nullptr) {
            existing->AddClaim(); // claim consumed by the caller's Subscribe
            return existing;
        }
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto existing = LookupFresh(key);
        if (existing != nullptr) {
            // Lost the race: discard the fresh context (no subscribers yet,
            // safe to close) and attach to the winner. The fresh session's
            // destructor runs after the lock is released.
            existing->AddClaim();
            return existing;
        }
        fresh->AddClaim(); // publisher claim, consumed by the caller's Subscribe
        Registry()[key] = fresh;
        fresh->key_ = key;
    }
    return fresh;
}

std::shared_ptr<DemuxSession> DemuxSession::Find(const std::string& src, const NetOptions& opts)
{
    const std::string key = BuildKey(src, opts);
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto it = Registry().find(key);
    if (it == Registry().end()) {
        return nullptr;
    }
    // Probe reuse: keep the context alive across the metadata read even when
    // the session has no subscribers left.
    return it->second.lock();
}

bool DemuxSession::Init(const std::string& src, const NetOptions& opts, std::string& err)
{
    src_ = src;
    opts_ = opts;
    // Arm the read-stall watchdog BEFORE the interrupt callback is attached:
    // open + find_stream_info already run under InterruptCb, and a zero
    // lastReadWallMs_ would abort them immediately. The playlist resolve
    // below also runs network I/O under the same callback.
    liveReadStallMs_ = opts.live ? LIVE_READ_STALL_MS : 0;
    lastReadWallMs_.store(NowSteadyMs(), std::memory_order_relaxed);
    readStallFired_.store(false, std::memory_order_relaxed);
    // A2/F1: resolve variant selection + latency-derived live start index.
    // Infallible: falls back to the original URL on any failure.
    const SourceResolution res = ResolveSource(src, opts, &DemuxSession::InterruptCb, this);
    // ABR (1.16): capture the variant ladder from the resolved master
    // playlist. Switching is armed only for live sources without an explicit
    // variantIndex pin; the published stats work whenever a variant was
    // selected (pinned included).
    if (opts.abr) {
        RefreshAbrLadder(res);
        abrActive_ = opts.live && opts.variantIndex == INT32_MIN && abrLadderPos_ >= 0;
        if (abrActive_) {
            abrDwellMs_ = ABR_UP_DWELL_MS;
            abrNextUpProbeMs_ = NowSteadyMs() + abrDwellMs_;
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                "abr armed: %{public}zu rungs, starting at doc#%{public}d bw=%{public}lld",
                abrLadder_.size(), AbrVariantIndex(),
                static_cast<long long>(AbrVariantBandwidthBps()));
        }
    }
    NetOptions effOpts = opts;
    if (res.liveStartIndex != INT32_MIN) {
        effOpts.liveStartIndex = res.liveStartIndex;
    }
    fmt_ = avformat_alloc_context();
    fmt_->interrupt_callback.callback = &DemuxSession::InterruptCb;
    fmt_->interrupt_callback.opaque = this;
    if (res.isHlsPlaylist) {
        ForceHlsInputFormat(fmt_);
    }
    AVDictionary* dict = nullptr;
    ApplyOpenOptions(fmt_, &dict, effOpts);
    if (RaiseLiveAnalyzeWindow(fmt_, effOpts, res.isHlsPlaylist)) {
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
            "live raw-transport source: analysis window raised to %{public}lldms "
            "(avformat_find_stream_info exits adaptively at the first IDR)",
            static_cast<long long>(LIVE_RAW_ANALYZE_CAP_MS));
    }
    std::string ioErr;
    if (!AttachCustomIO(fmt_, res.url, ioErr)) {
        if (!ioErr.empty()) {
            err = ioErr;
            avformat_free_context(fmt_);
            fmt_ = nullptr;
            return false;
        }
    }
    int ret = avformat_open_input(&fmt_, res.url.c_str(), nullptr, &dict);
    av_dict_free(&dict);
    if (ret < 0) {
        err = "avformat_open_input failed";
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    ret = avformat_find_stream_info(fmt_, nullptr);
    if (ret < 0) {
        err = "avformat_find_stream_info failed";
        AVIOContext* ioPb = fmt_->pb;
        avformat_close_input(&fmt_);
        ReleaseStreamIO(ioPb);
        return false;
    }
    if (opts.program != INT32_MIN) {
        program_ = SelectProgram(fmt_, opts.program);
        if (program_ == nullptr) {
            err = "program not found: " + std::to_string(opts.program);
            AVIOContext* ioPb = fmt_->pb;
            avformat_close_input(&fmt_);
            ReleaseStreamIO(ioPb);
            return false;
        }
    }
    SnapshotStreamLayout(streamCodecs_);
    // startTimeMs precise start: seek once per connection before the demux
    // thread starts. Attachers with a different (later) requested start
    // simply drop pre-target frames from the session's position.
    if (opts.startTimeMs > 0) {
        int64_t targetUs = static_cast<int64_t>(opts.startTimeMs) * 1000;
        if (avformat_seek_file(fmt_, -1, INT64_MIN, targetUs, INT64_MAX, 0) < 0) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                "seek to %{public}lldms failed, starting from head",
                static_cast<long long>(opts.startTimeMs));
        }
    }
    demuxAlive_.store(true, std::memory_order_relaxed);
    demuxThread_ = std::thread(&DemuxSession::DemuxLoop, this);
    return true;
}

int DemuxSession::InterruptCb(void* opaque)
{
    auto* self = static_cast<DemuxSession*>(opaque);
    if (self->stopping_.load(std::memory_order_relaxed)) {
        return 1;
    }
    if (self->starveAbort_.load(std::memory_order_relaxed)) {
        return 1;
    }
    if (self->liveReadStallMs_ > 0 &&
        NowSteadyMs() - self->lastReadWallMs_.load(std::memory_order_relaxed) >= self->liveReadStallMs_) {
        if (!self->readStallFired_.exchange(true)) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                "live read stall watchdog fired (%{public}lldms without a packet), aborting blocked read",
                static_cast<long long>(self->liveReadStallMs_));
        }
        return 1;
    }
    return 0;
}

std::vector<std::shared_ptr<PacketQueue>> DemuxSession::SnapshotSubscribers()
{
    std::lock_guard<std::mutex> lock(subsMtx_);
    std::vector<std::shared_ptr<PacketQueue>> subs;
    subs.reserve(subscribers_.size());
    for (auto& q : subscribers_) {
        subs.push_back(q);
    }
    return subs;
}

bool DemuxSession::RequestSharedSeek(int64_t targetMs)
{
    if (stopping_.load(std::memory_order_relaxed) ||
        !demuxAlive_.load(std::memory_order_relaxed)) {
        return false;
    }
    pendingSharedSeekMs_.store(targetMs, std::memory_order_release);
    return true;
}

AVStream* DemuxSession::FindSeekVideoStream() const
{
    if (fmt_ == nullptr) {
        return nullptr;
    }
    if (program_ != nullptr) {
        for (unsigned i = 0; i < program_->nb_stream_indexes; ++i) {
            const unsigned si = program_->stream_index[i];
            if (si < fmt_->nb_streams &&
                fmt_->streams[si]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return fmt_->streams[si];
            }
        }
        return nullptr;
    }
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return fmt_->streams[i];
        }
    }
    return nullptr;
}

void DemuxSession::DemuxLoop()
{
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux", "demux thread started");
    AVPacket* pkt = av_packet_alloc();
    int reconnectAttempts = 0;
    while (!stopping_.load(std::memory_order_relaxed)) {
        // ABR (1.16): apply an armed variant switch (starve-driven downgrade
        // or health-probe upgrade) before anything else — a pending shared
        // seek below then lands inside the NEW variant.
        if (pendingAbrSwitch_ != 0) {
            const int abrDir = pendingAbrSwitch_;
            pendingAbrSwitch_ = 0;
            if (!ApplyAbrSwitch(abrDir)) {
                auto eofSubs = SnapshotSubscribers();
                for (auto& q : eofSubs) {
                    q->PushEof();
                }
                break;
            }
        }
        // 鏂规鈶?shared-session seek: apply at the loop top (this thread is
        // the sole fmt_ owner). Last-write-wins; subscribers re-anchor via
        // NotifySeek and complete their pending seek acks on the epoch
        // advance. A failed seek leaves the demux position unchanged 鈥?
        // queues are still notified (ok=false) so armed acks fail fast
        // instead of timing out.
        const int64_t sharedSeekMs =
            pendingSharedSeekMs_.exchange(INT64_MIN, std::memory_order_acquire);
        if (sharedSeekMs != INT64_MIN) {
            const int sret = avformat_seek_file(fmt_, -1, INT64_MIN, sharedSeekMs * 1000, INT64_MAX, 0);
            const bool ok = sret >= 0;
            // Fast-landing: shrink the re-anchor target to the keyframe the
            // seek actually landed on when the pre-target gap is huge (see
            // FAST_SEEK_GAP_MS). Same index search the demuxer uses for the
            // backward seek itself, so the entry always matches the real
            // landing point; no match keeps the exact target.
            int64_t effectiveMs = sharedSeekMs;
            AVStream* vst = ok ? FindSeekVideoStream() : nullptr;
            if (vst != nullptr) {
                const int64_t targetTs =
                    av_rescale_q(sharedSeekMs, av_make_q(1, 1000), vst->time_base);
                const int keyIdx = av_index_search_timestamp(vst, targetTs, AVSEEK_FLAG_BACKWARD);
                if (keyIdx >= 0) {
                    const AVIndexEntry* entry = avformat_index_get_entry(vst, keyIdx);
                    if (entry != nullptr) {
                        const int64_t keyMs = av_rescale_q(entry->timestamp, vst->time_base, av_make_q(1, 1000));
                        if (sharedSeekMs - keyMs > FAST_SEEK_GAP_MS) {
                            effectiveMs = keyMs;
                        }
                    }
                }
            }
            if (ok) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                    "shared seek applied: target=%{public}lldms effective=%{public}lldms",
                    static_cast<long long>(sharedSeekMs), static_cast<long long>(effectiveMs));
            } else {
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                    "shared seek to %{public}lldms failed (ret=%{public}d)",
                    static_cast<long long>(sharedSeekMs), sret);
            }
            auto subs = SnapshotSubscribers();
            for (auto& q : subs) {
                q->NotifySeek(effectiveMs, ok);
            }
        }
        int ret = av_read_frame(fmt_, pkt);
        if (ret < 0) {
            // A3/A4: token expiry (401/403), sliding-window misses (404) and
            // rate limiting (429) can be healed by rebuilding the connection
            // from the original URL (fresh signed playlist). Give up after
            // the retry budget and fall through to the EOF path.
            if (TryRecover(ret, reconnectAttempts)) {
                continue;
            }
            // Hard EOF or unrecoverable read error: every subscriber sees a
            // sticky end marker (same semantics as a private connection).
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                "demux read end ret=%{public}d, delivering EOF", ret);
            auto subs = SnapshotSubscribers();
            for (auto& q : subs) {
                q->PushEof();
            }
            break;
        }
        reconnectAttempts = 0; // a delivered packet proves the link works
        auto subs = SnapshotSubscribers();
        // Media duration of this packet (ms) for subscriber buffer-depth
        // stats (BufferedAheadMs); 0 when unknown (no duration) 鈥?the stat
        // then undercounts but never overcounts.
        int64_t durationMs = 0;
        if (pkt->duration > 0 && pkt->stream_index >= 0 &&
            pkt->stream_index < static_cast<int>(fmt_->nb_streams)) {
            const AVStream* st = fmt_->streams[pkt->stream_index];
            durationMs = static_cast<int64_t>(pkt->duration) * 1000 * st->time_base.num /
                st->time_base.den;
        }
        for (auto& q : subs) {
            q->Route(pkt, durationMs);
        }
        // Throughput watchdog (live only): judge media-time progress before
        // the packet is unref'd (needs stream time_base / pts).
        if (liveReadStallMs_ > 0) {
            TrackStarveWatchdog(pkt);
        }
        av_packet_unref(pkt);
        // F1: shed excess backlog so live latency converges on the target.
        if (liveReadStallMs_ > 0 && opts_.targetLatencyMs > 0) {
            MaybeCatchUp();
        }
        // Read delivered: re-arm the live read-stall watchdog.
        lastReadWallMs_.store(NowSteadyMs(), std::memory_order_relaxed);
        readStallFired_.store(false, std::memory_order_relaxed);
    }
    av_packet_free(&pkt);
    demuxAlive_.store(false, std::memory_order_relaxed);
    // Bounded-join exit handoff (see DEMUX_JOIN_TIMEOUT_MS): publish the exit
    // and, when Shutdown already gave up on the join (orphan), take over the
    // fmt_ close it skipped. exitMtx_ serializes the decision with Shutdown:
    // exactly one side closes fmt_.
    {
        bool selfClose = false;
        {
            std::lock_guard<std::mutex> lock(exitMtx_);
            demuxExited_.store(true, std::memory_order_release);
            selfClose = orphanClose_.load(std::memory_order_acquire);
        }
        if (selfClose && fmt_ != nullptr) {
            AVIOContext* ioPb = fmt_->pb;
            avformat_close_input(&fmt_);
            ReleaseStreamIO(ioPb);
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                "orphaned demux thread closed the context after late exit");
        }
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux", "demux thread exited");
}

void DemuxSession::SnapshotStreamLayout(std::vector<int>& out)
{
    out.clear();
    if (fmt_ == nullptr) {
        return;
    }
    out.reserve(fmt_->nb_streams);
    for (unsigned int i = 0; i < fmt_->nb_streams; ++i) {
        out.push_back(fmt_->streams[i] != nullptr && fmt_->streams[i]->codecpar != nullptr
            ? static_cast<int>(fmt_->streams[i]->codecpar->codec_id)
            : -1);
    }
}

bool DemuxSession::Rebuild(std::string& err)
{
    // Demux-thread only (sole fmt_ owner).
    AVIOContext* oldIoPb = fmt_->pb;
    avformat_close_input(&fmt_);
    ReleaseStreamIO(oldIoPb);
    fmt_ = nullptr;
    program_ = nullptr;
    // Re-arm the watchdogs: the rebuild itself performs network I/O under
    // InterruptCb and the fresh connection deserves a clean slate.
    lastReadWallMs_.store(NowSteadyMs(), std::memory_order_relaxed);
    readStallFired_.store(false, std::memory_order_relaxed);
    starveAbort_.store(false, std::memory_order_relaxed);
    starveVideoStreamIndex_ = -1;
    starveWindowStartMs_ = 0;
    starveWindowProgressMs_ = 0.0;
    starveWindowVideoPkts_ = 0;
    starveLastVideoPts_ = AV_NOPTS_VALUE;
    starveLowSinceMs_ = 0;
    abrStarveSinceMs_ = 0;
    abrHealthySinceMs_ = 0;
    // Re-resolve from the ORIGINAL source: a token-protected CDN mints a
    // fresh signed variant URL on every master-playlist fetch (A3).
    const SourceResolution res = ResolveSource(src_, opts_, &DemuxSession::InterruptCb, this);
    NetOptions effOpts = opts_;
    if (res.liveStartIndex != INT32_MIN) {
        effOpts.liveStartIndex = res.liveStartIndex;
    }
    fmt_ = avformat_alloc_context();
    fmt_->interrupt_callback.callback = &DemuxSession::InterruptCb;
    fmt_->interrupt_callback.opaque = this;
    if (res.isHlsPlaylist) {
        ForceHlsInputFormat(fmt_);
    }
    AVDictionary* dict = nullptr;
    ApplyOpenOptions(fmt_, &dict, effOpts);
    if (RaiseLiveAnalyzeWindow(fmt_, effOpts, res.isHlsPlaylist)) {
        // Mid-stream reconnect lands mid-GOP just like a fresh open: same
        // adaptive window so the rebuilt session does not come back with
        // 0x0 video parameters.
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
            "rebuild: live raw-transport analysis window raised to %{public}lldms",
            static_cast<long long>(LIVE_RAW_ANALYZE_CAP_MS));
    }
    std::string ioErr;
    if (!AttachCustomIO(fmt_, res.url, ioErr)) {
        if (!ioErr.empty()) {
            err = ioErr;
            avformat_free_context(fmt_);
            fmt_ = nullptr;
            return false;
        }
    }
    int ret = avformat_open_input(&fmt_, res.url.c_str(), nullptr, &dict);
    av_dict_free(&dict);
    if (ret < 0) {
        err = "avformat_open_input failed: " + std::to_string(ret);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    ret = avformat_find_stream_info(fmt_, nullptr);
    if (ret < 0) {
        err = "avformat_find_stream_info failed";
        AVIOContext* ioPb = fmt_->pb;
        avformat_close_input(&fmt_);
        ReleaseStreamIO(ioPb);
        return false;
    }
    if (opts_.program != INT32_MIN) {
        program_ = SelectProgram(fmt_, opts_.program);
        if (program_ == nullptr) {
            err = "program not found after rebuild: " + std::to_string(opts_.program);
            AVIOContext* ioPb = fmt_->pb;
            avformat_close_input(&fmt_);
            ReleaseStreamIO(ioPb);
            return false;
        }
    }
    // Subscribers subscribe by stream index: a layout change would silently
    // mis-route packets into wrong decoders. Abort the recovery instead 鈥?
    // the EOF path lets the app rebuild its pipeline around the new layout.
    std::vector<int> codecs;
    SnapshotStreamLayout(codecs);
    if (codecs != streamCodecs_) {
        err = "stream layout changed after rebuild";
        AVIOContext* ioPb = fmt_->pb;
        avformat_close_input(&fmt_);
        ReleaseStreamIO(ioPb);
        return false;
    }
    // ABR (1.16): refresh the ladder from the freshly fetched master so doc
    // indexes stay valid when the CDN rotates its variant list between
    // fetches. The variant that just opened dropping off the ladder ends
    // switching (playback itself continues).
    if (abrActive_) {
        RefreshAbrLadder(res);
        if (abrLadderPos_ < 0) {
            abrActive_ = false;
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                "abr: current variant left the ladder, switching disabled");
        }
    }
    return true;
}

bool DemuxSession::TryRecover(int readErr, int& attempts)
{
    if (stopping_.load(std::memory_order_relaxed)) {
        return false;
    }
    // autoReconnect: -1 = auto (live on, VOD off).
    const bool autoReconnect = opts_.autoReconnect < 0 ? opts_.live : opts_.autoReconnect != 0;
    if (!autoReconnect) {
        return false;
    }
    const bool recoverable = readErr == AVERROR_HTTP_UNAUTHORIZED ||
        readErr == AVERROR_HTTP_FORBIDDEN || readErr == AVERROR_HTTP_NOT_FOUND ||
        readErr == AVERROR_HTTP_TOO_MANY_REQUESTS;
    if (!recoverable) {
        return false;
    }
    const int maxRetries = opts_.reconnectMaxRetries > 0 ? opts_.reconnectMaxRetries : 0;
    while (attempts < maxRetries && !stopping_.load(std::memory_order_relaxed)) {
        // Exponential backoff; 429 (rate limit) starts slower and climbs
        // higher. The classic avio path cannot read the Retry-After header,
        // so the schedule is our best guess (documented limitation).
        int64_t delayMs = readErr == AVERROR_HTTP_TOO_MANY_REQUESTS ? 2000 : 1000;
        for (int i = 0; i < attempts; ++i) {
            delayMs *= 2;
        }
        const int64_t capMs = readErr == AVERROR_HTTP_TOO_MANY_REQUESTS ? 16000 : 8000;
        if (delayMs > capMs) {
            delayMs = capMs;
        }
        attempts++;
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
            "recoverable http error %{public}d, rebuild attempt %{public}d/%{public}d in %{public}lldms",
            readErr, attempts, maxRetries, static_cast<long long>(delayMs));
        for (int64_t slept = 0; slept < delayMs && !stopping_.load(std::memory_order_relaxed);
            slept += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stopping_.load(std::memory_order_relaxed)) {
            return false;
        }
        std::string err;
        if (Rebuild(err)) {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                "session rebuilt after http error %{public}d (attempt %{public}d)", readErr, attempts);
            // Subscribers: count the reconnect (stats + 'reconnected'
            // event via worker polling) and discard the old-timeline
            // backlog; decode workers re-anchor on the next pts jump.
            auto subs = SnapshotSubscribers();
            for (auto& q : subs) {
                q->NotifyReconnect();
                q->DropBacklogToKeyframe();
            }
            return true;
        }
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
            "rebuild failed: %{public}s", err.c_str());
    }
    return false;
}

void DemuxSession::MaybeCatchUp()
{
    const int64_t now = NowSteadyMs();
    if (lastCatchupCheckMs_ != 0 && now - lastCatchupCheckMs_ < 5000) {
        return;
    }
    lastCatchupCheckMs_ = now;
    // Tolerance: only shed when clearly above target (target + max(2s,
    // target/2)) so the normal jitter band does not thrash the backlog.
    const int64_t tolerance = opts_.targetLatencyMs / 2 > 2000 ? opts_.targetLatencyMs / 2 : 2000;
    const int64_t threshold = opts_.targetLatencyMs + tolerance;
    auto subs = SnapshotSubscribers();
    for (auto& q : subs) {
        const int64_t buffered = q->BufferedAheadMs();
        if (buffered > threshold) {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                "latency catch-up: buffered %{public}lldms > target %{public}lldms + tol, dropping backlog",
                static_cast<long long>(buffered), static_cast<long long>(opts_.targetLatencyMs));
            q->DropBacklogToKeyframe();
        }
    }
}

void DemuxSession::TrackStarveWatchdog(const AVPacket* pkt)
{
    AVStream* st = (pkt->stream_index >= 0 &&
        pkt->stream_index < static_cast<int>(fmt_->nb_streams))
        ? fmt_->streams[pkt->stream_index] : nullptr;
    if (st == nullptr) {
        return;
    }
    // Track only the video stream: summing progress across all streams would
    // multiply realtime by the stream count and mask a partial starvation.
    if (starveVideoStreamIndex_ < 0) {
        if (st->codecpar == nullptr || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            return;
        }
        starveVideoStreamIndex_ = pkt->stream_index;
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
            "starve watchdog tracking video stream %{public}d", starveVideoStreamIndex_);
    }
    if (pkt->stream_index != starveVideoStreamIndex_) {
        return;
    }
    // Media-time progress of this packet in ms: prefer the demuxer-provided
    // duration (pts-jump immune); fall back to the positive pts delta with a
    // < 1s cap (excludes discontinuity jumps).
    double progressMs = 0.0;
    if (pkt->duration > 0) {
        progressMs = static_cast<double>(pkt->duration) * 1000.0 *
            static_cast<double>(st->time_base.num) / static_cast<double>(st->time_base.den);
    } else if (pkt->pts != AV_NOPTS_VALUE && starveLastVideoPts_ != AV_NOPTS_VALUE) {
        const int64_t delta = pkt->pts - starveLastVideoPts_;
        if (delta > 0 && delta < 90000) {
            progressMs = static_cast<double>(delta) * 1000.0 *
                static_cast<double>(st->time_base.num) / static_cast<double>(st->time_base.den);
        }
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
        starveLastVideoPts_ = pkt->pts;
    }
    if (starveWindowStartMs_ == 0) {
        starveWindowStartMs_ = NowSteadyMs();
    }
    starveWindowProgressMs_ += progressMs;
    starveWindowVideoPkts_++;
    const int64_t span = NowSteadyMs() - starveWindowStartMs_;
    if (span < LIVE_STARVE_WINDOW_MS) {
        return;
    }
    const double windowProgress = starveWindowProgressMs_;
    const double ratio = windowProgress / static_cast<double>(span);
    const int windowPkts = starveWindowVideoPkts_;
    starveWindowProgressMs_ = 0.0;
    starveWindowVideoPkts_ = 0;
    starveWindowStartMs_ = NowSteadyMs();
    // Progress unmeasurable (no duration, no usable pts deltas): judge by
    // packet presence instead 鈥?packets flowed, so the demux is not starved.
    if (windowProgress <= 0.0 && windowPkts > 0) {
        starveLowSinceMs_ = 0;
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
        "live progress ratio %{public}.2f over last %{public}lldms", ratio,
        static_cast<long long>(span));
    // ABR (1.16): per-window ladder evaluation (arm down/up switches) ahead
    // of the abort bookkeeping, so a downgrade can rescue the session before
    // LIVE_STARVE_MS kills it.
    if (abrActive_) {
        AbrEvaluateWindow(ratio);
    }
    if (ratio >= LIVE_STARVE_HEALTHY) {
        starveLowSinceMs_ = 0;
        return;
    }
    if (ratio > LIVE_STARVE_RATIO) {
        return; // transitional band: keep the current starve timer state
    }
    const int64_t now = NowSteadyMs();
    if (starveLowSinceMs_ == 0) {
        starveLowSinceMs_ = now;
        return;
    }
    if (now - starveLowSinceMs_ >= LIVE_STARVE_MS) {
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
            "live starve watchdog fired (progress ratio %{public}.2f for %{public}lldms), aborting session",
            ratio, static_cast<long long>(now - starveLowSinceMs_));
        starveAbort_.store(true, std::memory_order_relaxed);
    }
}

void DemuxSession::RefreshAbrLadder(const SourceResolution& res)
{
    // Demux-thread only.
    abrLadder_.clear();
    abrLadderBw_.clear();
    abrLadderPos_ = -1;
    if (res.selectedVariant < 0 ||
        res.selectedVariant >= static_cast<int>(res.variants.size())) {
        return;
    }
    abrVariantIndex_.store(res.selectedVariant, std::memory_order_relaxed);
    abrVariantBandwidth_.store(
        res.variants[static_cast<size_t>(res.selectedVariant)].bandwidth,
        std::memory_order_relaxed);
    // Ladder: known-bandwidth variants at/below maxBandwidth, sorted by
    // descending bandwidth; needs at least two rungs to be switchable.
    const int64_t cap = opts_.maxBandwidth > 0 ? opts_.maxBandwidth : INT64_MAX;
    std::vector<std::pair<int64_t, int>> rungs; // (bandwidth, doc index)
    for (size_t i = 0; i < res.variants.size(); ++i) {
        const int64_t bw = res.variants[i].bandwidth;
        if (bw > 0 && bw <= cap) {
            rungs.emplace_back(bw, static_cast<int>(i));
        }
    }
    if (rungs.size() < 2) {
        return;
    }
    std::sort(rungs.begin(), rungs.end(),
        [](const std::pair<int64_t, int>& a, const std::pair<int64_t, int>& b) {
            return a.first > b.first;
        });
    for (const auto& rung : rungs) {
        abrLadderBw_.push_back(rung.first);
        abrLadder_.push_back(rung.second);
        if (rung.second == res.selectedVariant) {
            abrLadderPos_ = static_cast<int>(abrLadder_.size()) - 1;
        }
    }
}

void DemuxSession::AbrEvaluateWindow(double ratio)
{
    // Demux-thread only; one call per completed LIVE_STARVE_WINDOW_MS window.
    const int64_t now = NowSteadyMs();
    if (ratio <= LIVE_STARVE_RATIO) {
        // Starved window: once the streak covers ABR_STARVE_SWITCH_MS (two
        // consecutive windows), arm a one-rung downgrade. Without a lower
        // rung the stock starve-abort path stays in charge.
        abrHealthySinceMs_ = 0;
        if (abrStarveSinceMs_ == 0) {
            abrStarveSinceMs_ = now;
            return;
        }
        if (now - abrStarveSinceMs_ >= ABR_STARVE_SWITCH_MS && abrLadderPos_ >= 0 &&
            abrLadderPos_ < static_cast<int>(abrLadder_.size()) - 1) {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                "abr: starve streak %{public}lldms (ratio %{public}.2f), arming downgrade from rung %{public}d",
                static_cast<long long>(now - abrStarveSinceMs_), ratio, abrLadderPos_);
            pendingAbrSwitch_ = -1; // overrides a pending upgrade probe
        }
        return;
    }
    abrStarveSinceMs_ = 0;
    if (ratio < ABR_UP_HEALTHY) {
        return; // transitional band: neither starved nor clearly healthy
    }
    if (abrHealthySinceMs_ == 0) {
        abrHealthySinceMs_ = now;
        return;
    }
    if (now - abrHealthySinceMs_ >= ABR_UP_STABLE_MS) {
        // Sustained health: lift any failed-probe penalty.
        abrDwellMs_ = ABR_UP_DWELL_MS;
    }
    if (pendingAbrSwitch_ == 0 && abrLadderPos_ > 0 && now >= abrNextUpProbeMs_) {
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
            "abr: healthy streak %{public}lldms (ratio %{public}.2f), arming upgrade probe from rung %{public}d",
            static_cast<long long>(now - abrHealthySinceMs_), ratio, abrLadderPos_);
        pendingAbrSwitch_ = 1;
        abrHealthySinceMs_ = 0; // restart the health streak on the new rung
    }
}

bool DemuxSession::ApplyAbrSwitch(int dir)
{
    // Demux-thread only (sole fmt_ owner). Returns false when the session
    // died (switch rebuild AND rollback rebuild both failed).
    if (!abrActive_ || stopping_.load(std::memory_order_relaxed)) {
        return true;
    }
    const int newPos = abrLadderPos_ + dir;
    if (newPos < 0 || newPos >= static_cast<int>(abrLadder_.size())) {
        return true; // no rung in that direction: keep the current variant
    }
    const int32_t prevVariant = opts_.variantIndex;
    const int prevPos = abrLadderPos_;
    const int docIndex = abrLadder_[static_cast<size_t>(newPos)];
    const int64_t bw = abrLadderBw_[static_cast<size_t>(newPos)];
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
        "abr: %{public}s to doc#%{public}d (bw=%{public}lld, rung %{public}d -> %{public}d)",
        dir < 0 ? "downgrading" : "upgrading", docIndex, static_cast<long long>(bw),
        prevPos, newPos);
    opts_.variantIndex = docIndex;
    std::string err;
    if (Rebuild(err)) {
        if (dir < 0) {
            // Failed-probe penalty: push the next upgrade attempt further out.
            abrDwellMs_ = abrDwellMs_ * 2 > ABR_UP_DWELL_MAX_MS
                ? ABR_UP_DWELL_MAX_MS : abrDwellMs_ * 2;
        }
        abrNextUpProbeMs_ = NowSteadyMs() + abrDwellMs_;
        abrSwitchCount_.fetch_add(1, std::memory_order_relaxed);
        // Subscribers: same recovery semantics as a token rebuild — count
        // the reconnect, drop the old-timeline backlog at the next keyframe.
        auto subs = SnapshotSubscribers();
        for (auto& q : subs) {
            q->NotifyReconnect();
            q->DropBacklogToKeyframe();
        }
        return true;
    }
    // The failed Rebuild closed fmt_: roll back to the previous variant or
    // the session dies with it.
    OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
        "abr: switch to doc#%{public}d failed (%{public}s), rolling back to doc#%{public}d",
        docIndex, err.c_str(), prevVariant);
    opts_.variantIndex = prevVariant;
    abrLadderPos_ = prevPos;
    if (Rebuild(err)) {
        return true;
    }
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFDemux",
        "abr: rollback to doc#%{public}d failed (%{public}s), ending session",
        prevVariant, err.c_str());
    return false;
}

std::shared_ptr<PacketQueue> DemuxSession::Subscribe(const std::vector<int>& streamIndexes)
{
    auto queue = std::make_shared<PacketQueue>();
    queue->SetStreams(streamIndexes);
    {
        std::lock_guard<std::mutex> lock(subsMtx_);
        if (claims_ > 0) {
            claims_--;
        }
        subscribers_.push_back(queue);
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
            "subscribed %{public}zu stream(s), subscribers=%{public}zu",
            streamIndexes.size(), subscribers_.size());
    }
    return queue;
}

void DemuxSession::CancelClaim()
{
    std::lock_guard<std::mutex> lock(subsMtx_);
    if (claims_ > 0) {
        claims_--;
    }
}

void DemuxSession::AddClaim()
{
    std::lock_guard<std::mutex> lock(subsMtx_);
    claims_++;
}

bool DemuxSession::HasSubscribersOrClaims()
{
    std::lock_guard<std::mutex> lock(subsMtx_);
    return claims_ > 0 || !subscribers_.empty();
}

void DemuxSession::DetachQueue(PacketQueue* queue)
{
    std::lock_guard<std::mutex> lock(subsMtx_);
    for (size_t i = 0; i < subscribers_.size(); ++i) {
        if (subscribers_[i].get() == queue) {
            subscribers_[i]->Detach();
            subscribers_.erase(subscribers_.begin() + static_cast<long>(i));
            break;
        }
    }
}

void DemuxSession::Shutdown()
{
    // Order matters: arm the interrupt first (a demux thread blocked in a
    // network read aborts), then detach queues (a demux thread blocked on a
    // full queue wakes), then join.
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(subsMtx_);
        for (auto& q : subscribers_) {
            q->Detach();
        }
        subscribers_.clear();
        claims_ = 0;
    }
    if (demuxThread_.joinable()) {
        // Bounded join: a live read can legitimately sit in av_read_frame for
        // up to LIVE_READ_STALL_MS (30 s) before the stall watchdog aborts
        // it. Blocking the NAPI caller (JS main thread) that long recreates
        // the teardown-interlock appfreeze (THREAD_BLOCK_6S). Wait a short
        // bounded window; past it detach the thread and let its own
        // watchdogs end it — the orphan exit path closes fmt_ itself.
        const int64_t deadline = NowSteadyMs() + DEMUX_JOIN_TIMEOUT_MS;
        while (!demuxExited_.load(std::memory_order_acquire) &&
            NowSteadyMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> lock(exitMtx_);
        if (demuxExited_.load(std::memory_order_acquire)) {
            demuxThread_.join();
        } else {
            orphanClose_.store(true, std::memory_order_release);
            demuxThread_.detach();
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                "demux thread stuck past %{public}dms join budget, detached (orphan close)",
                DEMUX_JOIN_TIMEOUT_MS);
        }
    }
    if (fmt_ != nullptr && !orphanClose_.load(std::memory_order_acquire)) {
        AVIOContext* ioPb = fmt_->pb;
        avformat_close_input(&fmt_);
        ReleaseStreamIO(ioPb);
    }
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto it = Registry().find(key_);
        if (it != Registry().end()) {
            auto registered = it->second.lock();
            if (registered.get() == this) {
                Registry().erase(it);
            }
        }
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux", "session closed");
}

DemuxSession::~DemuxSession()
{
    Shutdown();
}

} // namespace ffmpeg
