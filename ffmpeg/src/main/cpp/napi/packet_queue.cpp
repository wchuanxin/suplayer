#include "packet_queue.h"

#include <chrono>

#ifdef FFMPEG_HOST_TEST
#include <cstdio>
#define PQ_LOG(level, fmt, ...) \
    do { std::fprintf(stderr, "[FFDemux] " fmt "\n", ##__VA_ARGS__); } while (0)
#else
#include <hilog/log.h>
#define PQ_LOG(level, fmt, ...) \
    OH_LOG_Print(LOG_APP, level, 0xFF00, "FFDemux", fmt, ##__VA_ARGS__)
#endif

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
// CPU-starved one (app startup contention, system stalls) within a few
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

int64_t NowSteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

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
            QueuedPacket front = packets_.front();
            packets_.pop_front();
            bytes_ -= static_cast<size_t>(front.pkt->size);
            av_packet_move_ref(out, front.pkt);
            av_packet_free(&front.pkt);
            popped_ = true;
            lastPopWallMs_ = NowSteadyMs(); // consumer alive: re-arm the grace wait
            if (out->stream_index == statsStream_.load(std::memory_order_relaxed)) {
                bufferedSumMs_ -= front.durationMs;
            }
            bufferedMs_.store(bufferedSumMs_, std::memory_order_relaxed);
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
            notEmpty_.wait_for(lock,
                remainMs < 100 ? std::chrono::milliseconds(remainMs) : POLL_MS);
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

void PacketQueue::SetPolicy(const BackpressurePolicy& policy)
{
    std::lock_guard<std::mutex> lock(mtx_);
    policy_ = policy;
}

void PacketQueue::SetStatsStream(int streamIndex)
{
    statsStream_.store(streamIndex, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mtx_);
    PublishBufferedMsLocked();
}

void PacketQueue::PublishBufferedMsLocked()
{
    const int stats = statsStream_.load(std::memory_order_relaxed);
    int64_t sum = 0;
    for (const QueuedPacket& q : packets_) {
        if (q.pkt->stream_index == stats) {
            sum += q.durationMs;
        }
    }
    bufferedSumMs_ = sum;
    bufferedMs_.store(sum, std::memory_order_relaxed);
}

void PacketQueue::DropBacklogLocked(const AVPacket* trigger)
{
    const size_t dropped = packets_.size();
    for (QueuedPacket& q : packets_) {
        av_packet_free(&q.pkt);
    }
    packets_.clear();
    bytes_ = 0;
    skipUntilKey_ = policy_.skipToKeyframe &&
        (trigger == nullptr || (trigger->flags & AV_PKT_FLAG_KEY) == 0);
    overflowEvents_.fetch_add(1, std::memory_order_relaxed);
    droppedPackets_.fetch_add(dropped, std::memory_order_relaxed);
    PublishBufferedMsLocked();
    PQ_LOG(LOG_WARN, "live queue overflow: dropped %{public}zu packets, resume at %{public}s",
        dropped, skipUntilKey_ ? "next keyframe" : "next packet");
}

void PacketQueue::Route(const AVPacket* pkt, int64_t durationMs)
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
    if (packets_.size() >= packetCap || bytes_ >= QUEUE_HIGH_WATER_BYTES) {
        if (!policy_.liveDrop) {
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
                DropBacklogLocked(pkt);
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
                DropBacklogLocked(pkt);
            }
        }
    }
    if (skipUntilKey_) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            // Keyframe-skip gap after a backlog drop: discarded packets are
            // part of the same overflow episode (no new OverflowEvents
            // increment), but they count toward the dropped-packet total.
            droppedPackets_.fetch_add(1, std::memory_order_relaxed);
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
    QueuedPacket queued;
    queued.pkt = copy;
    queued.durationMs = durationMs;
    packets_.push_back(queued);
    bytes_ += static_cast<size_t>(copy->size);
    if (copy->stream_index == statsStream_.load(std::memory_order_relaxed)) {
        bufferedSumMs_ += durationMs;
    }
    bufferedMs_.store(bufferedSumMs_, std::memory_order_relaxed);
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
        for (QueuedPacket& q : packets_) {
            av_packet_free(&q.pkt);
        }
        packets_.clear();
        bytes_ = 0;
        PublishBufferedMsLocked();
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
    if (policy_.skipToKeyframe) {
        // Keep from the LAST queued keyframe onward — the newest clean
        // restart point closest to the live edge.
        for (size_t i = packets_.size(); i > 0; --i) {
            if ((packets_[i - 1].pkt->flags & AV_PKT_FLAG_KEY) != 0) {
                dropUntil = i - 1;
                break;
            }
        }
    }
    for (size_t i = 0; i < dropUntil; ++i) {
        av_packet_free(&packets_[i].pkt);
    }
    const size_t dropped = dropUntil;
    if (dropUntil > 0 && dropUntil < packets_.size()) {
        packets_.erase(packets_.begin(), packets_.begin() + static_cast<long>(dropUntil));
    } else if (dropUntil == packets_.size()) {
        packets_.clear();
    }
    bytes_ = 0;
    for (const QueuedPacket& q : packets_) {
        bytes_ += static_cast<size_t>(q.pkt->size);
    }
    // Resync drops are app-initiated (flushToKeyframe / flushBacklog): they
    // count toward the dropped-packet total but are NOT overflow episodes —
    // they surface as their own 'discontinuity' event instead.
    droppedPackets_.fetch_add(dropped, std::memory_order_relaxed);
    PublishBufferedMsLocked();
    if (packets_.empty()) {
        // No keyframe queued: Route() drops packets until the next keyframe
        // arrives (policy_.skipToKeyframe) or passes everything through
        // (audio).
        skipUntilKey_ = policy_.skipToKeyframe;
    }
    // The consumer is about to resume from the edge: re-arm the live grace
    // window so the demux thread keeps pacing instead of dropping.
    lastPopWallMs_ = NowSteadyMs();
    PQ_LOG(LOG_INFO, "live resync: dropped %{public}zu backlog packets, resume at %{public}s",
        dropped, packets_.empty() ? "next keyframe" : "queued keyframe");
}

} // namespace ffmpeg
