#include "demux_session.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <hilog/log.h>

#include <chrono>

namespace ffmpeg {
namespace {

// Per-subscriber queue bounds. Consumers pace in real time, so queues hover
// near empty; the watermark only absorbs bursts (e.g. the 200 ms A/V-sync
// freeze) before backpressuring the demux thread.
constexpr size_t QUEUE_HIGH_WATER_PACKETS = 300;
constexpr size_t QUEUE_HIGH_WATER_BYTES = 64 * 1024 * 1024;
// Pre-start grace cap: covers the open->start rendezvous window (a couple of
// seconds of stream) without letting an abandoned subscriber grow unbounded.
constexpr size_t QUEUE_PRESTART_PACKETS = 4000;
// Live backpressure grace: while the consumer popped within this window the
// demux thread waits for it to drain one slot before dropping the backlog.
// A real-time consumer frees a slot every packet interval (~30-40 ms), a
// CPU-starved one (app startup contention, system-side stalls) within a few
// hundred ms — both pace the demux behind them with stable latency and no
// timeline jump. Only a consumer with no pop for this long (paused decoder,
// hung renderer) exhausts it and falls back to the immediate stuck drop.
constexpr int64_t LIVE_STUCK_MS = 3000;
// Non-live backpressure deadline: a VOD consumer that has not drained a slot
// for this long is dead or hung (e.g. its decode thread wedged elsewhere).
// Blocking the shared demux read forever would starve every other subscriber
// of the session — drop the backlog instead and let the stuck consumer see a
// timeline jump rather than a whole-session freeze.
constexpr int64_t NONLIVE_STUCK_MS = 30000;
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

// ============================ PacketQueue ============================

PacketQueue::~PacketQueue()
{
    // The subscriber dropped the queue without DetachQueue (e.g. decoder
    // destroyed): release buffered packets and make sure the demux thread is
    // not blocked on us.
    Detach();
}

int PacketQueue::Pop(AVPacket* out, std::atomic<bool>& abort, int64_t deadlineSteadyMs)
{
    // Re-check abort at a small fixed interval instead of relying on a wake
    // notification: the abort SETTER (decoder stop on the main thread) cannot
    // race-free notify this queue on its own, so a lost wakeup must recover
    // within POLL_MS. The cost (a few spurious wakeups per idle subscriber)
    // is negligible.
    constexpr auto POLL_MS = std::chrono::milliseconds(100);
    std::unique_lock<std::mutex> lock(mtx_);
    while (true) {
        if (!packets_.empty()) {
            AVPacket* front = packets_.front();
            packets_.pop_front();
            bytes_ -= static_cast<size_t>(front->size);
            av_packet_move_ref(out, front);
            av_packet_free(&front);
            popped_ = true;
            lastPopWallMs_ = NowSteadyMs(); // consumer alive: re-arm the grace wait
            notFull_.notify_all();
            return 0;
        }
        // Empty: end marker, abort or wait — EOF is sticky so the subscriber
        // drain loop keeps failing fast after the first end signal.
        if (eof_ || detached_ || abort.load(std::memory_order_relaxed)) {
            return -1;
        }
        if (deadlineSteadyMs > 0) {
            const int64_t now = NowSteadyMs();
            if (now >= deadlineSteadyMs) {
                return -1;
            }
            const int64_t remainMs = deadlineSteadyMs - now;
            notEmpty_.wait_for(lock, remainMs < 100 ? std::chrono::milliseconds(remainMs) : POLL_MS);
        } else {
            notEmpty_.wait_for(lock, POLL_MS);
        }
    }
}

bool PacketQueue::WantsStream(int streamIndex) const
{
    for (int si : streams_) {
        if (si == streamIndex) {
            return true;
        }
    }
    return false;
}

void PacketQueue::SetStreams(const std::vector<int>& streams)
{
    std::lock_guard<std::mutex> lock(mtx_);
    streams_ = streams;
}

void PacketQueue::SetLiveDrop(bool enabled, bool skipToKeyframe)
{
    std::lock_guard<std::mutex> lock(mtx_);
    liveDrop_ = enabled;
    skipToKey_ = skipToKeyframe;
}

void PacketQueue::Route(const AVPacket* pkt)
{
    std::unique_lock<std::mutex> lock(mtx_);
    if (detached_ || !WantsStream(pkt->stream_index)) {
        return;
    }
    // Startup grace: until the consumer has popped once (decode thread not
    // released yet — e.g. the audio side of the video/audio open rendezvous),
    // buffer up to the pre-start cap without backpressuring so a not-yet-
    // started subscriber cannot starve the others. Once consuming, the normal
    // high-water mark paces the shared read exactly like a private one.
    const size_t packetCap = popped_ ? QUEUE_HIGH_WATER_PACKETS : QUEUE_PRESTART_PACKETS;
    auto dropBacklog = [this, pkt]() {
        const size_t dropped = packets_.size();
        for (AVPacket* p : packets_) {
            av_packet_free(&p);
        }
        packets_.clear();
        bytes_ = 0;
        skipUntilKey_ = skipToKey_ && (pkt->flags & AV_PKT_FLAG_KEY) == 0;
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
            "live queue overflow: dropped %{public}zu packets, resume at %{public}s",
            dropped, skipToKey_ ? "next keyframe" : "next packet");
    };
    if (packets_.size() >= packetCap || bytes_ >= QUEUE_HIGH_WATER_BYTES) {
        if (!liveDrop_) {
            // Backpressure: hold the demux thread until the subscriber drains,
            // but never forever — a dead consumer must not freeze the whole
            // shared session (see NONLIVE_STUCK_MS).
            const int64_t deadlineMs = NowSteadyMs() + NONLIVE_STUCK_MS;
            while (!detached_ &&
                (packets_.size() >= packetCap || bytes_ >= QUEUE_HIGH_WATER_BYTES)) {
                if (notFull_.wait_until(lock,
                        std::chrono::steady_clock::time_point(std::chrono::milliseconds(deadlineMs))) ==
                    std::cv_status::timeout) {
                    break;
                }
            }
            if (detached_) {
                return;
            }
            if (packets_.size() >= packetCap || bytes_ >= QUEUE_HIGH_WATER_BYTES) {
                dropBacklog();
            }
        } else {
            // Live catch-up: pace behind the subscriber while it is alive.
            // The wait deadline is anchored to the LAST POP, not to this fill:
            // a healthy consumer (pop every packet interval) or even a heavily
            // CPU-starved one (startup contention, system stalls — pop every
            // few hundred ms) frees a slot well inside the window, so the
            // demux thread paces behind it, the queue stays pegged at the
            // high-water mark with stable latency and NO backlog drop, so the
            // timeline never jumps and downstream (decoder pacing anchors,
            // app write queues) never sees a burst. A consumer with no pop
            // for LIVE_STUCK_MS (paused / hung renderer) has a deadline
            // already in the past: every full-queue packet drops immediately
            // — re-waiting per packet would crawl the shared demux read and
            // starve the other subscribers (audio silence) and push the HLS
            // reader off the live edge (whole-segment skips). The backlog
            // drop lets video resume at the next keyframe (clean IDR
            // restart), audio at the next packet (independently decodable).
            const int64_t deadlineMs =
                (lastPopWallMs_ > 0 ? lastPopWallMs_ : NowSteadyMs()) + LIVE_STUCK_MS;
            while (!detached_ &&
                (packets_.size() >= packetCap || bytes_ >= QUEUE_HIGH_WATER_BYTES)) {
                if (NowSteadyMs() >= deadlineMs) {
                    break;
                }
                const auto deadline = std::chrono::steady_clock::time_point(
                    std::chrono::milliseconds(deadlineMs));
                if (notFull_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    break;
                }
            }
            if (detached_) {
                return;
            }
            if (packets_.size() >= packetCap || bytes_ >= QUEUE_HIGH_WATER_BYTES) {
                dropBacklog();
            }
        }
    }
    if (skipUntilKey_) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            return;
        }
        skipUntilKey_ = false;
    }
    AVPacket* copy = av_packet_alloc();
    if (av_packet_ref(copy, pkt) < 0) {
        // Out of memory: drop the packet rather than kill the pipeline.
        av_packet_free(&copy);
        return;
    }
    packets_.push_back(copy);
    bytes_ += static_cast<size_t>(copy->size);
    notEmpty_.notify_one();
}

void PacketQueue::PushEof()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        eof_ = true;
    }
    notEmpty_.notify_all();
}

void PacketQueue::Detach()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        detached_ = true;
        for (AVPacket* p : packets_) {
            av_packet_free(&p);
        }
        packets_.clear();
        bytes_ = 0;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
}

void PacketQueue::Wake()
{
    notEmpty_.notify_all();
}

void PacketQueue::DropBacklogToKeyframe()
{
    std::lock_guard<std::mutex> lock(mtx_);
    size_t dropUntil = packets_.size(); // default: drop everything
    if (skipToKey_) {
        // Keep from the LAST queued keyframe onward — the newest clean
        // restart point closest to the live edge.
        for (size_t i = packets_.size(); i > 0; --i) {
            if ((packets_[i - 1]->flags & AV_PKT_FLAG_KEY) != 0) {
                dropUntil = i - 1;
                break;
            }
        }
    }
    for (size_t i = 0; i < dropUntil; ++i) {
        av_packet_free(&packets_[i]);
    }
    const size_t dropped = dropUntil;
    if (dropUntil > 0 && dropUntil < packets_.size()) {
        packets_.erase(packets_.begin(), packets_.begin() + static_cast<long>(dropUntil));
    } else if (dropUntil == packets_.size()) {
        packets_.clear();
    }
    bytes_ = 0;
    for (AVPacket* p : packets_) {
        bytes_ += static_cast<size_t>(p->size);
    }
    if (packets_.empty()) {
        // No keyframe queued: Route() drops packets until the next keyframe
        // arrives (skipToKey_) or passes everything through (audio).
        skipUntilKey_ = skipToKey_;
    }
    // The consumer is about to resume from the edge: re-arm the live grace
    // window so the demux thread keeps pacing instead of dropping.
    lastPopWallMs_ = NowSteadyMs();
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
        "live resync: dropped %{public}zu backlog packets, resume at %{public}s",
        dropped, packets_.empty() ? "next keyframe" : "queued keyframe");
}

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
    // Arm the read-stall watchdog BEFORE the interrupt callback is attached:
    // open + find_stream_info already run under InterruptCb, and a zero
    // lastReadWallMs_ would abort them immediately.
    liveReadStallMs_ = opts.live ? LIVE_READ_STALL_MS : 0;
    lastReadWallMs_.store(NowSteadyMs(), std::memory_order_relaxed);
    readStallFired_.store(false, std::memory_order_relaxed);
    fmt_ = avformat_alloc_context();
    fmt_->interrupt_callback.callback = &DemuxSession::InterruptCb;
    fmt_->interrupt_callback.opaque = this;
    AVDictionary* dict = nullptr;
    ApplyOpenOptions(fmt_, &dict, opts);
    int ret = avformat_open_input(&fmt_, src.c_str(), nullptr, &dict);
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
    while (!stopping_.load(std::memory_order_relaxed)) {
        int ret = av_read_frame(fmt_, pkt);
        if (ret < 0) {
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
        auto subs = SnapshotSubscribers();
        for (auto& q : subs) {
            q->Route(pkt);
        }
        // Throughput watchdog (live only): judge media-time progress before
        // the packet is unref'd (needs stream time_base / pts).
        if (liveReadStallMs_ > 0) {
            TrackStarveWatchdog(pkt);
        }
        av_packet_unref(pkt);
        // Read delivered: re-arm the live read-stall watchdog.
        lastReadWallMs_.store(NowSteadyMs(), std::memory_order_relaxed);
        readStallFired_.store(false, std::memory_order_relaxed);
    }
    av_packet_free(&pkt);
    demuxAlive_.store(false, std::memory_order_relaxed);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux", "demux thread exited");
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
