#include "demux_session.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <hilog/log.h>

#include <chrono>

namespace ffmpeg {
namespace {

// Live read-stall watchdog: a live connection that has not delivered a
// single packet for this long is dead (server closed silently / network
// black hole) — av_read_frame would block forever because the interrupt
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
// progress per wall-clock window — immune to prestart bursts (ratio >> 1) and
// to pts jumps (per-packet duration, not pts deltas).
constexpr int64_t LIVE_STARVE_WINDOW_MS = 10000;
constexpr double LIVE_STARVE_RATIO = 0.15;   // < 15% of realtime = starving
constexpr double LIVE_STARVE_HEALTHY = 0.5;  // >= 50% of realtime = healthy
constexpr int64_t LIVE_STARVE_MS = 15000;    // sustained starve before abort

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
    key += "|lat=" + std::to_string(opts.targetLatencyMs);
    key += "|arc=" + std::to_string(opts.autoReconnect);
    key += "|rcm=" + std::to_string(opts.reconnectMaxRetries);
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
        // The parallel fresh open failed — e.g. the server enforces a single
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
    NetOptions effOpts = opts;
    if (res.liveStartIndex != INT32_MIN) {
        effOpts.liveStartIndex = res.liveStartIndex;
    }
    fmt_ = avformat_alloc_context();
    fmt_->interrupt_callback.callback = &DemuxSession::InterruptCb;
    fmt_->interrupt_callback.opaque = this;
    AVDictionary* dict = nullptr;
    ApplyOpenOptions(fmt_, &dict, effOpts);
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
        avformat_close_input(&fmt_);
        return false;
    }
    if (opts.program != INT32_MIN) {
        program_ = SelectProgram(fmt_, opts.program);
        if (program_ == nullptr) {
            err = "program not found: " + std::to_string(opts.program);
            avformat_close_input(&fmt_);
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

void DemuxSession::DemuxLoop()
{
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux", "demux thread started");
    AVPacket* pkt = av_packet_alloc();
    int reconnectAttempts = 0;
    while (!stopping_.load(std::memory_order_relaxed)) {
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
        // stats (BufferedAheadMs); 0 when unknown (no duration) — the stat
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
    avformat_close_input(&fmt_);
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
    AVDictionary* dict = nullptr;
    ApplyOpenOptions(fmt_, &dict, effOpts);
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
        avformat_close_input(&fmt_);
        return false;
    }
    if (opts_.program != INT32_MIN) {
        program_ = SelectProgram(fmt_, opts_.program);
        if (program_ == nullptr) {
            err = "program not found after rebuild: " + std::to_string(opts_.program);
            avformat_close_input(&fmt_);
            return false;
        }
    }
    // Subscribers subscribe by stream index: a layout change would silently
    // mis-route packets into wrong decoders. Abort the recovery instead —
    // the EOF path lets the app rebuild its pipeline around the new layout.
    std::vector<int> codecs;
    SnapshotStreamLayout(codecs);
    if (codecs != streamCodecs_) {
        err = "stream layout changed after rebuild";
        avformat_close_input(&fmt_);
        return false;
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
    // packet presence instead — packets flowed, so the demux is not starved.
    if (windowProgress <= 0.0 && windowPkts > 0) {
        starveLowSinceMs_ = 0;
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
        "live progress ratio %{public}.2f over last %{public}lldms", ratio,
        static_cast<long long>(span));
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
        demuxThread_.join();
    }
    if (fmt_ != nullptr) {
        avformat_close_input(&fmt_);
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
