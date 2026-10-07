#ifndef FFMPEG_NAPI_DEMUX_SESSION_H
#define FFMPEG_NAPI_DEMUX_SESSION_H

#include "hls_playlist.h"
#include "net_options.h"
#include "packet_queue.h"

#include <atomic>
#include <cstdint>
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
    // A3: rebuild the connection in place after a recoverable HTTP error.
    // Closes fmt_, re-resolves the source (fresh CDN token) and reopens.
    // Demux-thread only. Returns false when the stream layout changed (the
    // subscriber stream indexes would dangle) or the reopen failed.
    bool Rebuild(std::string& err);
    // A3/A4: decide whether a read error is recoverable and run the bounded
    // backoff+rebuild loop. Returns true when fmt_ is valid and the demux
    // loop should continue; false when the session must end (EOF path).
    // `attempts` carries the retry budget across loop iterations.
    bool TryRecover(int readErr, int& attempts);
    // F1: periodically drop subscriber backlogs that exceed the target
    // latency + tolerance (runtime catch-up toward the live edge).
    void MaybeCatchUp();
    void Shutdown();
    void AddClaim();
    bool HasSubscribersOrClaims();
    // Snapshot of the stream layout (codec ids by stream index) used to
    // validate an in-place rebuild: subscribers subscribe by index, so a
    // changed layout aborts the recovery instead of mis-routing packets.
    void SnapshotStreamLayout(std::vector<int>& out);

    static int InterruptCb(void* opaque);
    static std::string BuildKey(const std::string& src, const NetOptions& opts);
    static std::shared_ptr<DemuxSession> LookupFresh(const std::string& key);
    std::vector<std::shared_ptr<PacketQueue>> SnapshotSubscribers();

    AVFormatContext* fmt_ = nullptr;
    AVProgram* program_ = nullptr;
    std::string key_;
    // Original open parameters (A3 rebuild re-resolves from these so a
    // token-protected CDN mints a fresh signed variant URL).
    std::string src_;
    NetOptions opts_;
    std::vector<int> streamCodecs_; // layout guard for Rebuild()

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
    // F1 runtime catch-up: wall ms of the last BufferedAheadMs check
    // (0 = none yet); only armed when targetLatencyMs > 0.
    int64_t lastCatchupCheckMs_ = 0;
    std::thread demuxThread_;
};

} // namespace ffmpeg

#endif
