#ifndef FFMPEG_NAPI_HW_DECODER_H
#define FFMPEG_NAPI_HW_DECODER_H

extern "C" {
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <native_window/external_window.h>
}

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ffmpeg {

// Surface-mode hardware video decoder backed by OH_AVCodec (libnative_media_vdec.so).
// Decoded frames are rendered straight to the OHNativeWindow by the codec service,
// no CPU copies involved. Input must be Annex-B (in-band parameter sets, e.g. MPEG-TS).
//
// Threading model:
//  - FeedPacket() is called from the demux thread; it blocks until the codec
//    reports a free input buffer (via OnNeedInputBuffer on a service thread).
//  - OnNewOutputBuffer (service thread) enqueues {index, pts, flags}.
//  - A dedicated render thread paces against the first-output wall clock and
//    calls OH_VideoDecoder_RenderOutputBuffer at the due time (same pacing
//    semantics as the software path).
//
// Stop() ordering: stop_ + condvar notify -> FeedPacket/render thread exit ->
// alive_ cleared under lock -> OH_VideoDecoder_Stop -> OH_VideoDecoder_Destroy.
// All codec callbacks check alive_ under the same lock before touching state.
class HwVideoDecoder {
public:
    std::function<void(double ptsMs)> onFrame;
    std::function<void()> onEnd;
    std::function<void(const std::string& msg)> onError;

    HwVideoDecoder() = default;
    ~HwVideoDecoder();

    HwVideoDecoder(const HwVideoDecoder&) = delete;
    HwVideoDecoder& operator=(const HwVideoDecoder&) = delete;

    // mime: OH_AVCODEC_MIMETYPE_VIDEO_AVC / OH_AVCODEC_MIMETYPE_VIDEO_HEVC.
    // The window is not owned by this class (caller keeps it alive until Stop()).
    bool Start(OHNativeWindow* window, const char* mime, int width, int height, double fps);

    // Push one Annex-B access unit. eos=true flushes the decoder (data may be null).
    // Returns false when stopped before a buffer became available.
    bool FeedPacket(const uint8_t* data, int size, int64_t ptsUs, bool eos);

    void Pause() { paused_ = true; }
    void Resume() { paused_ = false; }
    void Stop();

    const std::string& GetLastError() const { return lastError_; }

private:
    struct InputItem {
        uint32_t index = 0;
        OH_AVBuffer* buffer = nullptr;
    };
    struct OutputItem {
        uint32_t index = 0;
        int64_t ptsUs = 0;
        uint32_t flags = 0;
    };

    static void OnErrorCb(OH_AVCodec* codec, int32_t code, void* userData);
    static void OnStreamChangedCb(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
    static void OnNeedInputCb(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData);
    static void OnNewOutputCb(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData);

    void RenderLoop();
    void Fail(const std::string& msg);

    OH_AVCodec* codec_ = nullptr;
    OHNativeWindow* window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::string lastError_;

    std::mutex mtx_;
    std::condition_variable inputCv_;
    std::condition_variable outputCv_;
    std::deque<InputItem> freeInputs_;
    std::deque<OutputItem> pendingOutputs_;

    std::thread renderThread_;
    std::atomic<bool> alive_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> paused_{ false };
};

} // namespace ffmpeg

#endif
