// Host-side unit tests for PacketQueue. Runs on the development machine
// (no OHOS SDK): the FFmpeg packet API is stubbed, hilog goes to stderr.
// Build & run via tools/host-tests.sh.
#include "packet_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using ffmpeg::BackpressurePolicy;
using ffmpeg::PacketQueue;

namespace {

int g_failures = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

int64_t SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A stack packet with static payload; Route() copies it (av_packet_ref), so
// the caller never transfers ownership.
void FillPacket(AVPacket* p, int streamIndex, int64_t durationMs, bool key)
{
    static uint8_t payload[16] = { 0xAB };
    p->stream_index = streamIndex;
    p->duration = durationMs;
    p->flags = key ? AV_PKT_FLAG_KEY : 0;
    p->size = 16;
    p->data = payload;
    p->pts = 0;
}

// Pops one packet and releases the moved heap payload.
bool PopRelease(PacketQueue& q, std::atomic<bool>& abort, AVPacket* out)
{
    if (q.Pop(out, abort) != 0) {
        return false;
    }
    av_packet_unref(out);
    return true;
}

// Marks the queue as consuming (leaves the startup-grace window) so the
// normal high-water mark (300) applies in the overflow tests.
void StartConsuming(PacketQueue& q, std::atomic<bool>& abort)
{
    AVPacket src;
    FillPacket(&src, 1, 0, true);
    q.Route(&src, 0);
    AVPacket out;
    Check(PopRelease(q, abort, &out), "startup pop");
}

void TestRoutePopFiltering()
{
    std::printf("TestRoutePopFiltering\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1 });

    AVPacket src;
    FillPacket(&src, 0, 10, false);
    q.Route(&src, 0); // not subscribed: dropped silently
    for (int i = 0; i < 3; ++i) {
        FillPacket(&src, 1, 10, i == 0);
        src.pts = i;
        q.Route(&src, 0);
    }

    AVPacket out;
    Check(PopRelease(q, abort, &out), "pop first subscribed packet");
    Check(out.pts == 0 && out.flags & AV_PKT_FLAG_KEY, "first packet is the keyframe");
    Check(PopRelease(q, abort, &out) && out.pts == 1, "pop second packet");
    Check(PopRelease(q, abort, &out) && out.pts == 2, "pop third packet");

    // Empty, no EOF, no abort: a bounded deadline ends the pop.
    const int64_t deadline = SteadyMs() + 150;
    Check(q.Pop(&out, abort, deadline) != 0, "deadline pop fails");
    Check(SteadyMs() >= deadline, "deadline honored");

    // EOF is sticky and fails fast.
    q.PushEof();
    Check(q.Pop(&out, abort) != 0, "pop after EOF fails");
    Check(q.Pop(&out, abort) != 0, "EOF is sticky");
}

void TestBufferedAheadMs()
{
    std::printf("TestBufferedAheadMs\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1, 2 });
    q.SetStatsStream(1);

    AVPacket src;
    Check(q.BufferedAheadMs() == 0, "empty queue: 0 ms");
    for (int i = 0; i < 10; ++i) {
        FillPacket(&src, 1, 40, false);
        q.Route(&src, 40);
    }
    for (int i = 0; i < 5; ++i) {
        FillPacket(&src, 2, 100, false); // not the stats stream
        q.Route(&src, 100);
    }
    Check(q.BufferedAheadMs() == 400, "stats-stream packets only");
    AVPacket out;
    for (int i = 0; i < 3; ++i) {
        Check(PopRelease(q, abort, &out), "pop");
    }
    Check(q.BufferedAheadMs() == 280, "pop decreases buffered ms");
    q.SetStatsStream(-1);
    Check(q.BufferedAheadMs() == 0, "stats disabled: 0 ms");
    q.SetStatsStream(1);
    Check(q.BufferedAheadMs() == 280, "re-enable recomputes from queue");
}

void TestLiveOverflowDrop()
{
    std::printf("TestLiveOverflowDrop\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1 });
    q.SetStatsStream(1);
    BackpressurePolicy policy;
    policy.liveDrop = true;
    policy.skipToKeyframe = true;
    q.SetPolicy(policy);
    StartConsuming(q, abort);

    AVPacket src;
    FillPacket(&src, 1, 10, true);
    for (int i = 0; i < 300; ++i) { // fill to the high-water mark
        q.Route(&src, 10);
    }
    Check(q.BufferedAheadMs() == 3000, "queue full: 3000 ms buffered");

    // One more route with a live policy and a just-popped consumer: the
    // grace window (3s) must elapse before the backlog drop fires.
    FillPacket(&src, 1, 10, false); // non-key trigger
    const int64_t t0 = SteadyMs();
    q.Route(&src, 10);
    const int64_t took = SteadyMs() - t0;
    Check(took >= 2500, "grace window waited before dropping");
    Check(q.OverflowEvents() == 1, "one overflow episode");
    Check(q.DroppedPackets() == 301, "300 backlog + non-key trigger dropped");
    Check(q.BufferedAheadMs() == 0, "backlog gone");

    // skipUntilKey: non-key packets are discarded until the next keyframe.
    FillPacket(&src, 1, 10, false);
    q.Route(&src, 10);
    Check(q.DroppedPackets() == 302 && q.BufferedAheadMs() == 0, "non-key skipped");
    Check(q.OverflowEvents() == 1, "skip gap is not a new episode");
    FillPacket(&src, 1, 10, true);
    q.Route(&src, 10);
    Check(q.BufferedAheadMs() == 10, "keyframe queued again");
}

void TestBackpressureBlocksUntilPop()
{
    std::printf("TestBackpressureBlocksUntilPop\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1 });
    q.SetStatsStream(1);
    BackpressurePolicy policy; // liveDrop = false: blocking backpressure
    q.SetPolicy(policy);
    StartConsuming(q, abort);

    AVPacket src;
    FillPacket(&src, 1, 10, true);
    for (int i = 0; i < 300; ++i) {
        q.Route(&src, 10);
    }

    std::atomic<bool> routed{ false };
    std::thread router([&q, &src, &routed]() {
        q.Route(&src, 10);
        routed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(!routed.load(), "Route blocks on a full queue");
    AVPacket out;
    Check(PopRelease(q, abort, &out), "pop frees a slot");
    router.join();
    Check(routed.load(), "Route resumed after pop");
    Check(q.BufferedAheadMs() == 3000, "queue full again (300 x 10ms)");
}

void TestDropBacklogToKeyframe()
{
    std::printf("TestDropBacklogToKeyframe\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1 });
    q.SetStatsStream(1);
    BackpressurePolicy policy;
    policy.skipToKeyframe = true;
    q.SetPolicy(policy);
    StartConsuming(q, abort);

    AVPacket src;
    // key, n, n, key, n, n — the resync must keep from the LAST keyframe.
    const bool keys[6] = { true, false, false, true, false, false };
    for (int i = 0; i < 6; ++i) {
        FillPacket(&src, 1, 10, keys[i]);
        q.Route(&src, 10);
    }
    q.DropBacklogToKeyframe();
    Check(q.DroppedPackets() == 3, "resync dropped 3 pre-keyframe packets");
    Check(q.OverflowEvents() == 0, "resync is not an overflow episode");
    Check(q.BufferedAheadMs() == 30, "kept 3 packets from the last keyframe");

    // The queue now STARTS at the kept keyframe: another resync keeps it (a
    // re-anchor to the newest keyframe is a no-op there). Pop the keyframe
    // so the queue holds no keyframe at all, then the resync drops
    // everything and arms the keyframe skip.
    AVPacket out;
    Check(PopRelease(q, abort, &out), "pop the resync keyframe");
    Check(q.BufferedAheadMs() == 20, "20 ms left after popping the keyframe");
    q.DropBacklogToKeyframe();
    Check(q.DroppedPackets() == 5 && q.BufferedAheadMs() == 0, "keyframe-less resync emptied");
    FillPacket(&src, 1, 10, false);
    q.Route(&src, 10);
    Check(q.DroppedPackets() == 6, "non-key still skipped after empty resync");
    FillPacket(&src, 1, 10, true);
    q.Route(&src, 10);
    Check(q.BufferedAheadMs() == 10, "keyframe queued after skip gap");
}

void TestDetachUnblocksPop()
{
    std::printf("TestDetachUnblocksPop\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1 });

    std::atomic<bool> popped{ false };
    std::thread popper([&q, &abort, &popped]() {
        AVPacket out;
        q.Pop(&out, abort); // no deadline: polls until woken
        popped.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(!popped.load(), "Pop waits on an empty queue");
    q.Detach();
    popper.join();
    Check(popped.load(), "Detach unblocked Pop");

    AVPacket out;
    Check(q.Pop(&out, abort) != 0, "pop on detached queue fails fast");
}

void TestAbortWithWake()
{
    std::printf("TestAbortWithWake\n");
    PacketQueue q;
    std::atomic<bool> abort{ false };
    q.SetStreams({ 1 });

    std::atomic<bool> popped{ false };
    std::thread popper([&q, &abort, &popped]() {
        AVPacket out;
        q.Pop(&out, abort);
        popped.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    abort.store(true);
    q.Wake();
    popper.join();
    Check(popped.load(), "abort + Wake ends Pop promptly");
}

} // namespace

int main()
{
    TestRoutePopFiltering();
    TestBufferedAheadMs();
    TestLiveOverflowDrop();
    TestBackpressureBlocksUntilPop();
    TestDropBacklogToKeyframe();
    TestDetachUnblocksPop();
    TestAbortWithWake();
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
