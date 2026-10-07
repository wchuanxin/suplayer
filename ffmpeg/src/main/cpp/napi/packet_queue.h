#ifndef FFMPEG_NAPI_PACKET_QUEUE_H
#define FFMPEG_NAPI_PACKET_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
}

namespace ffmpeg {

// Overflow behavior of a subscriber queue when it is at its high-water mark.
struct BackpressurePolicy {
    // Live catch-up: drop the backlog instead of blocking the demux thread
    // (a blocked Route() would starve every other subscriber of the shared
    // session).
    bool liveDrop = false;
    // After a backlog drop, resume at the next keyframe (video: clean IDR
    // restart). Off = resume at the next packet (audio frames are
    // independently decodable).
    bool skipToKeyframe = false;
};

// Bounded blocking packet queue between the demux thread and one subscriber.
class PacketQueue {
public:
    PacketQueue() = default;
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // Blocking pop for the subscriber decode thread. Returns av_read_frame
    // compatible status: 0 = packet returned (ownership moved into `out`,
    // caller unrefs), <0 = end of stream OR aborted (subscriber stop flag /
    // queue detached). EOF is sticky: once the end marker arrives, every
    // subsequent pop fails fast. `deadlineSteadyMs` (0 = none) bounds the
    // wait; a deadline hit also returns <0 (used by the hw-eligibility peek
    // on streams that are declared but never carry data).
    int Pop(AVPacket* out, std::atomic<bool>& abort, int64_t deadlineSteadyMs = 0);

    // Demux-thread side. Route() drops packets outside the subscribed set;
    // a full queue blocks the demux thread (backpressure) or drops the
    // backlog, per the policy. `durationMs` is the packet's media duration
    // in ms (0 when unknown); packets of the stats stream feed
    // BufferedAheadMs().
    void SetStreams(const std::vector<int>& streams);
    void SetPolicy(const BackpressurePolicy& policy);
    void Route(const AVPacket* pkt, int64_t durationMs);
    void PushEof();
    void Detach();

    // Subscriber-thread side: wake a blocked Pop so it re-checks its abort
    // flag promptly (stop while the queue is empty and the demux thread is
    // stalled would otherwise sleep until the next packet). Idempotent and
    // harmless when no Pop is waiting.
    void Wake();

    // Control-thread side (A/V resync): drop the entire backlog and resume
    // at the NEWEST keyframe still queued (video), or at the next routed
    // packet when no keyframe is queued / skipToKey is off (audio). Used to
    // re-anchor a lagging track to the live edge when the decoded tracks
    // have drifted apart by more than freeze-based correction can close.
    void DropBacklogToKeyframe();

    // ---- Buffer stats -------------------------------------------------
    // Only packets of this stream index contribute to BufferedAheadMs()
    // (the subscriber's primary stream; a queue may subscribe several, e.g.
    // all audio tracks). -1 = duration accounting disabled. Switching
    // recomputes the buffered total from the currently queued packets.
    void SetStatsStream(int streamIndex);

    // Media duration of the currently buffered stats-stream packets, in ms.
    // A snapshot (atomic); updated on every route/pop/drop.
    int64_t BufferedAheadMs() const
    {
        return bufferedMs_.load(std::memory_order_relaxed);
    }

    // Overflow episodes: backlog drops caused by a full queue. App-initiated
    // resync drops (DropBacklogToKeyframe) do NOT count — those surface as
    // their own 'discontinuity' event.
    uint64_t OverflowEvents() const
    {
        return overflowEvents_.load(std::memory_order_relaxed);
    }

    // Total packets discarded on this queue: overflow backlog drops, the
    // keyframe-skip gap after a drop, and resync drops. Teardown (Detach)
    // does not count.
    uint64_t DroppedPackets() const
    {
        return droppedPackets_.load(std::memory_order_relaxed);
    }

private:
    struct QueuedPacket {
        AVPacket* pkt = nullptr;
        int64_t durationMs = 0; // media duration of the packet (any stream)
    };

    bool WantsStream(int streamIndex) const;
    // Drop the whole backlog (overflow path). `trigger` is the packet that
    // found the queue full (may be nullptr for tests). Caller holds mtx_.
    void DropBacklogLocked(const AVPacket* trigger);
    // Recompute bufferedSumMs_ from the queued packets of the stats stream
    // and publish the snapshot. Caller holds mtx_.
    void PublishBufferedMsLocked();

    std::mutex mtx_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<QueuedPacket> packets_;
    std::vector<int> streams_;
    size_t bytes_ = 0;
    bool eof_ = false;
    bool detached_ = false;
    // True once the consumer has popped at least once. Until then the queue
    // is in its startup grace window: the subscriber has subscribed but its
    // decode thread has not been released yet (A1 video/audio rendezvous,
    // hw/sw decision), and it must not backpressure the shared demux read —
    // the other subscribers would starve behind a queue nobody drains.
    bool popped_ = false;
    BackpressurePolicy policy_;
    // After a backlog drop with policy_.skipToKeyframe, Route() discards
    // packets until the next keyframe arrives.
    bool skipUntilKey_ = false;
    // Steady-clock ms of the consumer's last Pop (0 before the first pop).
    // A full live queue paces the demux behind the consumer only while the
    // consumer is alive: once now - lastPopWallMs_ exceeds LIVE_STUCK_MS the
    // consumer is deemed stuck (paused / hung) and every full-queue packet
    // drops immediately — per-packet grace waits would crawl the shared demux
    // read and starve the other subscribers. The next Pop re-arms the wait.
    int64_t lastPopWallMs_ = 0;

    // Stats stream index (see SetStatsStream); only its packets feed the
    // buffered-ms total.
    std::atomic<int> statsStream_{ -1 };
    // Running sum of the queued stats-stream packets' media duration, in ms.
    // Guarded by mtx_; mirrored to the atomic snapshot below on every change.
    int64_t bufferedSumMs_ = 0;
    std::atomic<int64_t> bufferedMs_{ 0 };
    std::atomic<uint64_t> overflowEvents_{ 0 };
    std::atomic<uint64_t> droppedPackets_{ 0 };
};

} // namespace ffmpeg

#endif
