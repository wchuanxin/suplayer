#include "hw_decoder.h"

extern "C" {
#include <multimedia/player_framework/native_avformat.h>
#include <hilog/log.h>
}

#include <chrono>
#include <cstring>

namespace ffmpeg {

HwVideoDecoder::~HwVideoDecoder()
{
    Stop();
}

void HwVideoDecoder::Fail(const std::string& msg)
{
    lastError_ = msg;
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideoHW", "%{public}s", msg.c_str());
    if (codec_ != nullptr) {
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
}

bool HwVideoDecoder::CheckCapability(const char* mime, int width, int height, double fps, int profile,
    std::string& reason)
{
    OH_AVCapability* cap = OH_AVCodec_GetCapabilityByCategory(mime, false, HARDWARE);
    if (cap == nullptr) {
        reason = std::string("no hardware decoder for mime=") + mime;
        return false;
    }
    const char* name = OH_AVCapability_GetName(cap);
    const char* safeName = (name != nullptr) ? name : "?";
    if (width > 0 && height > 0 && !OH_AVCapability_IsVideoSizeSupported(cap, width, height)) {
        reason = "hw decoder " + std::string(safeName) + " does not support size " +
            std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    if (profile >= 0) {
        const int32_t* profiles = nullptr;
        uint32_t profileNum = 0;
        if (OH_AVCapability_GetSupportedProfiles(cap, &profiles, &profileNum) == AV_ERR_OK &&
            profileNum > 0 && profiles != nullptr) {
            bool found = false;
            for (uint32_t i = 0; i < profileNum; ++i) {
                if (profiles[i] == profile) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                reason = "hw decoder " + std::string(safeName) + " does not support profile=" +
                    std::to_string(profile);
                return false;
            }
        }
    }
    if (width > 0 && height > 0 && fps > 0.0) {
        // Container frame-rate metadata is advisory and often inaccurate; a
        // mismatch is logged but does not block the hardware path.
        int32_t fpsInt = static_cast<int32_t>(fps + 0.5);
        if (!OH_AVCapability_AreVideoSizeAndFrameRateSupported(cap, width, height, fpsInt)) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideoHW",
                "hw codec %{public}s: %{public}dx%{public}d @%{public}d fps outside reported frame-rate range, trying anyway",
                safeName, width, height, fpsInt);
        }
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideoHW",
        "hw capability ok: codec=%{public}s %{public}dx%{public}d profile=%{public}d",
        safeName, width, height, profile);
    return true;
}

bool HwVideoDecoder::Start(OHNativeWindow* window, const char* mime, int width, int height, double fps)
{
    window_ = window;
    width_ = width;
    height_ = height;
    stop_ = false;
    paused_ = false;

    codec_ = OH_VideoDecoder_CreateByMime(mime);
    if (codec_ == nullptr) {
        Fail(std::string("create hw decoder failed, mime=") + mime);
        return false;
    }
    OH_AVCodecCallback cb = { OnErrorCb, OnStreamChangedCb, OnNeedInputCb, OnNewOutputCb };
    if (OH_VideoDecoder_RegisterCallback(codec_, cb, this) != AV_ERR_OK) {
        Fail("register hw callback failed");
        return false;
    }
    OH_AVFormat* fmt = OH_AVFormat_Create();
    if (fmt == nullptr) {
        Fail("create avformat failed");
        return false;
    }
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    if (fps > 0.0) {
        OH_AVFormat_SetDoubleValue(fmt, OH_MD_KEY_FRAME_RATE, fps);
    }
    OH_AVErrCode ret = OH_VideoDecoder_Configure(codec_, fmt);
    OH_AVFormat_Destroy(fmt);
    if (ret != AV_ERR_OK) {
        Fail("hw configure failed ret=" + std::to_string(ret));
        return false;
    }
    // SetSurface is only legal in the Configured state (or later), i.e. after
    // Configure and before Prepare.
    if (OH_VideoDecoder_SetSurface(codec_, window_) != AV_ERR_OK) {
        Fail("hw set surface failed");
        return false;
    }
    if (OH_VideoDecoder_Prepare(codec_) != AV_ERR_OK) {
        Fail("hw prepare failed");
        return false;
    }
    if (OH_VideoDecoder_Start(codec_) != AV_ERR_OK) {
        Fail("hw start failed");
        return false;
    }
    alive_ = true;
    renderThread_ = std::thread(&HwVideoDecoder::RenderLoop, this);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideoHW",
        "hw decoder started mime=%{public}s %{public}dx%{public}d fps=%{public}.2f", mime, width, height, fps);
    return true;
}

bool HwVideoDecoder::FeedPacket(const uint8_t* data, int size, int64_t ptsUs, bool eos)
{
    if (stop_.load() || codec_ == nullptr) {
        return false;
    }
    InputItem in {};
    {
        std::unique_lock<std::mutex> lock(mtx_);
        // The codec normally returns free input buffers continuously; a timeout
        // guards against a stalled/hung decoder service so the demux thread
        // cannot block forever (Stop() would otherwise never join it).
        bool ok = inputCv_.wait_for(lock, std::chrono::seconds(10),
            [this] { return stop_.load() || !freeInputs_.empty(); });
        if (stop_.load() || freeInputs_.empty()) {
            if (!stop_.load() && onError) {
                onError("hw decoder input stalled (no free buffer in 10s)");
            }
            return false;
        }
        in = freeInputs_.front();
        freeInputs_.pop_front();
    }
    if (!eos && data != nullptr && size > 0) {
        int32_t cap = OH_AVBuffer_GetCapacity(in.buffer);
        if (size > cap) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideoHW",
                "input overflow size=%{public}d cap=%{public}d (truncated)", size, cap);
            size = cap;
        }
        uint8_t* addr = OH_AVBuffer_GetAddr(in.buffer);
        if (addr != nullptr) {
            memcpy(addr, data, static_cast<size_t>(size));
        }
    }
    OH_AVCodecBufferAttr attr;
    attr.pts = eos ? 0 : ptsUs;
    attr.size = eos ? 0 : size;
    attr.offset = 0;
    attr.flags = eos ? AVCODEC_BUFFER_FLAGS_EOS : AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(in.buffer, &attr);
    OH_AVErrCode ret = OH_VideoDecoder_PushInputBuffer(codec_, in.index);
    if (ret != AV_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideoHW", "push input buffer failed ret=%{public}d", ret);
        return false;
    }
    return true;
}

void HwVideoDecoder::Stop()
{
    if (codec_ == nullptr && !renderThread_.joinable()) {
        return;
    }
    stop_ = true;
    inputCv_.notify_all();
    outputCv_.notify_all();
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
    if (codec_ != nullptr) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            alive_ = false;
            freeInputs_.clear();
            pendingOutputs_.clear();
        }
        OH_VideoDecoder_Stop(codec_);
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideoHW", "hw decoder stopped");
}

void HwVideoDecoder::OnErrorCb(OH_AVCodec* codec, int32_t code, void* userData)
{
    (void)codec;
    auto* self = static_cast<HwVideoDecoder*>(userData);
    std::string msg = "hw decoder error code=" + std::to_string(code);
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "FFVideoHW", "%{public}s", msg.c_str());
    if (self->onError != nullptr) {
        self->onError(msg);
    }
}

void HwVideoDecoder::OnStreamChangedCb(OH_AVCodec* codec, OH_AVFormat* format, void* userData)
{
    (void)codec;
    (void)userData;
    int32_t w = 0;
    int32_t h = 0;
    if (format != nullptr) {
        OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w);
        OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideoHW", "stream changed %{public}dx%{public}d", w, h);
}

void HwVideoDecoder::OnNeedInputCb(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData)
{
    (void)codec;
    auto* self = static_cast<HwVideoDecoder*>(userData);
    {
        std::lock_guard<std::mutex> lock(self->mtx_);
        if (!self->alive_.load()) {
            return;
        }
        self->freeInputs_.push_back(InputItem { index, buffer });
    }
    self->inputCv_.notify_one();
}

void HwVideoDecoder::OnNewOutputCb(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData)
{
    (void)codec;
    auto* self = static_cast<HwVideoDecoder*>(userData);
    OH_AVCodecBufferAttr attr;
    attr.pts = 0;
    attr.size = 0;
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if (buffer != nullptr) {
        OH_AVBuffer_GetBufferAttr(buffer, &attr);
    }
    {
        std::lock_guard<std::mutex> lock(self->mtx_);
        if (!self->alive_.load()) {
            return;
        }
        self->pendingOutputs_.push_back(OutputItem { index, attr.pts, attr.flags });
    }
    self->outputCv_.notify_one();
}

void HwVideoDecoder::RenderLoop()
{
    int64_t firstPtsUs = INT64_MIN;
    auto startWall = std::chrono::steady_clock::now();
    int rendered = 0;

    while (true) {
        OutputItem item {};
        {
            std::unique_lock<std::mutex> lock(mtx_);
            outputCv_.wait(lock, [this] { return stop_.load() || !pendingOutputs_.empty(); });
            if (pendingOutputs_.empty()) {
                if (stop_.load()) {
                    break;
                }
                continue;
            }
            item = pendingOutputs_.front();
            pendingOutputs_.pop_front();
        }

        if ((item.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
            OH_VideoDecoder_FreeOutputBuffer(codec_, item.index);
            if (onEnd != nullptr) {
                onEnd();
            }
            break;
        }
        if (stop_.load()) {
            OH_VideoDecoder_FreeOutputBuffer(codec_, item.index);
            continue;
        }
        while (paused_.load() && !stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stop_.load()) {
            OH_VideoDecoder_FreeOutputBuffer(codec_, item.index);
            continue;
        }

        // real-time pacing based on pts delta (same semantics as the software path)
        if (firstPtsUs == INT64_MIN) {
            firstPtsUs = item.ptsUs;
            startWall = std::chrono::steady_clock::now();
        } else if (item.ptsUs > firstPtsUs) {
            double targetMs = static_cast<double>(item.ptsUs - firstPtsUs) / 1000.0;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startWall).count();
            int64_t waitMs = static_cast<int64_t>(targetMs) - static_cast<int64_t>(elapsed);
            if (waitMs > 0 && waitMs < 1000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            }
        }

        OH_AVErrCode ret = OH_VideoDecoder_RenderOutputBuffer(codec_, item.index);
        if (ret != AV_ERR_OK) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFVideoHW", "render output buffer failed ret=%{public}d", ret);
        } else {
            rendered++;
        }
        if (onFrame != nullptr) {
            onFrame(static_cast<double>(item.ptsUs) / 1000.0);
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFVideoHW", "render done rendered=%{public}d", rendered);
}

} // namespace ffmpeg
