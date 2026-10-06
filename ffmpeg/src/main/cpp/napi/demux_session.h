#ifndef FFMPEG_NAPI_DEMUX_SESSION_H
#define FFMPEG_NAPI_DEMUX_SESSION_H

#include "net_options.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ffmpeg {

// B1 shared demuxer: a process-global registry of DemuxSession objects keyed
// by (src + demux-relevant options). One AVFormatContext and one network
// connection serve every decoder (video / audio / subtitle) of the same
// source, removing the per-decoder connection bandwidth amplification
// (video + audio + subtitle + probe each used to open their own connection).
//
// The first decoder creates the session: open input, analyze stream info,
// lock the MPEG-TS program, apply the startTimeMs seek, publish to the
// registry, spawn the demux thread. Concurrent first-openers race; the loser
// discards its fresh context and attaches to the winner (keeps startup
// parallelism, single steady-state connection).
//
// Each decoder subscribes a set of stream indexes and receives packets
// through a private bounded queue; the demux thread routes every packet to
// the queues subscribed to its stream index. Queue high-water backpressure
// paces the demux thread on the slowest subscriber — the same self-pacing
// semantics as today's per-connection decoders (a paused decoder stalls the
// shared read exactly like it used to stall its private read).
//
// Staleness: a session with zero subscribers AND zero claims (e.g. lingering
// only in a transient probe reference after a seek reload) is never reused —
// GetOrCreate replaces it so a new open (with a new startTimeMs) always gets
// a fresh connection positioned at its requested start.
//
// Threading:
//  - GetOrCreate / Subscribe / DetachQueue are called from NAPI worker
//    threads; the registry mutex serializes publication and claims.
//  - The demux thread is the only av_read_frame caller on the context.
//  - Subscriber decode threads pop from their own queue and read stable
//    AVStream metadata (time_base / codecpar); av_read_frame never mutates
//    those fields, so concurrent metadata reads are safe.
//  - Teardown runs on whichever thread releases the last reference: mark
//    stopping (interrupt callback aborts a blocked network read), detach all
//    queues (unblocks a demux thread waiting on a full queue), join the
//    demux thread, close the context.

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
    // Push blocks while the queue is at its high-water mark (backpressure).
    void SetStreams(const std::vector<int>& streams);
    void Route(const AVPacket* pkt);
    void PushEof();
    void Detach();

    // Subscriber-thread side, before the decode thread starts popping.
    // Live catch-up mode: when the queue is already at its high-water mark,
    // instead of blocking the demux thread (which would starve every other
    // subscriber of the shared session) the backlog is dropped. Video queues
    // resume at the next keyframe (clean IDR restart); audio queues resume at
    // the next packet (audio frames are independently decodable). The audio
    // variant also covers a consumer that never pops at all — e.g. the app
    // renderer start hangs while the app is backgrounded — which must not
    // deadlock the shared demux read on backpressure.
    void SetLiveDrop(bool enabled, bool skipToKeyframe);

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

private:
    bool WantsStream(int streamIndex) const;

    std::mutex mtx_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<AVPacket*> packets_;
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
    // Live catch-up policy (see SetLiveDrop).
    bool liveDrop_ = false;
    bool skipToKey_ = false;
    bool skipUntilKey_ = false;
    // Steady-clock ms of the consumer's last Pop (0 before the first pop).
    // A full live queue paces the demux behind the consumer only while the
    // consumer is alive: once now - lastPopWallMs_ exceeds LIVE_STUCK_MS the
    // consumer is deemed stuck (paused / hung) and every full-queue packet
    // drops immediately — per-packet grace waits would crawl the shared demux
    // read and starve the other subscribers. The next Pop re-arms the wait.
    int64_t lastPopWallMs_ = 0;
};

class DemuxSession {
public:
    // Get or create the session for (src + demux options). On success the
    // caller holds one CLAIM: it must promptly either Subscribe() (consumes
    // the claim) or CancelClaim() (open failed midway). Returns nullptr and
    // fills `err` when a fresh open fails.
    static std::shared_ptr<DemuxSession> GetOrCreate(const std::string& src, const NetOptions& opts,
        std::string& err);

    // Find an already active session (probe metadata reuse). Returns nullptr
    // when no live session exists. The returned reference keeps the context
    // alive until released.
    static std::shared_ptr<DemuxSession> Find(const std::string& src, const NetOptions& opts);

    ~DemuxSession();

    DemuxSession(const DemuxSession&) = delete;
    DemuxSession& operator=(const DemuxSession&) = delete;

    // Subscribe a set of stream indexes; packets of those streams are routed
    // to the returned queue. Consumes the caller's claim. Call at most once
    // per GetOrCreate.
    std::shared_ptr<PacketQueue> Subscribe(const std::vector<int>& streamIndexes);

    // Give up a claim without subscribing (decoder open failed after
    // GetOrCreate). A session left with neither subscribers nor claims is
    // considered stale and is replaced by the next GetOrCreate.
    void CancelClaim();

    // Detach a previously returned queue (subscriber closing). The queue
    // object itself is owned by the subscriber's shared_ptr; DetachQueue only
    // drops the session's routing reference and unblocks the demux thread.
    void DetachQueue(PacketQueue* queue);

    AVFormatContext* fmt() { return fmt_; }
    AVProgram* program() { return program_; }

private:
    DemuxSession() = default;

    bool Init(const std::string& src, const NetOptions& opts, std::string& err);
    void DemuxLoop();
    // Live throughput watchdog bookkeeping (see members below). Called per
    // demuxed packet from the demux thread only.
    void TrackStarveWatchdog(const AVPacket* pkt);
    void Shutdown();
    void AddClaim();
    bool HasSubscribersOrClaims();

    static int InterruptCb(void* opaque);
    static std::string BuildKey(const std::string& src, const NetOptions& opts);
    static std::shared_ptr<DemuxSession> LookupFresh(const std::string& key);
    std::vector<std::shared_ptr<PacketQueue>> SnapshotSubscribers();

    AVFormatContext* fmt_ = nullptr;
    AVProgram* program_ = nullptr;
    std::string key_;

    std::mutex subsMtx_;
    std::vector<std::shared_ptr<PacketQueue>> subscribers_;
    int claims_ = 0;

    std::atomic<bool> stopping_{ false };
    // True while the demux thread is running. Once it exits (EOF / read
    // error) the session is a corpse: subscribers still drain queued
    // packets, but LookupFresh must not hand it to a new open — a fresh
    // session with a live demux thread is created instead.
    std::atomic<bool> demuxAlive_{ false };
    // Live read-stall watchdog: 0 (disabled) for non-live sources; for live
    // sources InterruptCb aborts a blocked av_read_frame once no packet has
    // been delivered for this long (see LIVE_READ_STALL_MS).
    int64_t liveReadStallMs_ = 0;
    std::atomic<int64_t> lastReadWallMs_{ 0 };
    std::atomic<bool> readStallFired_{ false };
    // Live throughput watchdog: a throttled connection (server rate-limiting
    // same-uid concurrent downloads) still TRICKLES packets, so the no-packet
    // watchdog never fires while playback starves in slow motion. This one
    // measures the media-time progress of the video stream per wall-clock
    // window; if progress stays under LIVE_STARVE_RATIO of realtime for
    // LIVE_STARVE_MS, the session is aborted via the same recovery path
    // (InterruptCb returns 1 → av_read_frame fails → EOF to subscribers).
    int starveVideoStreamIndex_ = -1; // first video stream seen; -1 = none
    int64_t starveWindowStartMs_ = 0;
    double starveWindowProgressMs_ = 0.0;
    int starveWindowVideoPkts_ = 0; // packets seen in the current window
    int64_t starveLastVideoPts_ = AV_NOPTS_VALUE; // fallback progress source
    int64_t starveLowSinceMs_ = 0; // 0 = window rate currently not starved
    std::atomic<bool> starveAbort_{ false };
    std::thread demuxThread_;
};

} // namespace ffmpeg

#endif
