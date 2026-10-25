#include "napi/native_api.h"
#include "probe.h"
#include "frame_extractor.h"
#include "audio_decoder.h"
#include "video_decoder.h"
#include "subtitle_decoder.h"
#include "transcoder.h"
#include "stream_io.h"

extern "C" {
#include <libavutil/log.h>
#include <hilog/log.h>
}

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace {
// GOP 中段加入（直播裸流/共享会话附加）时，h264 软解在带内 SPS/PPS+IDR 到达前会对
// 每个 slice 刷解码错误（实测 20ms 内数百条 "non-existing PPS 0 referenced"/
// "decode_slice_header error"/"no frame!"），冲垮 hilog 且无诊断价值。按前缀识别此类
// 噪音并限流：首次放行，其后每 5s 至多放行 1 条并附累计抑制计数，保留诊断能力。
constexpr int NOISE_LOG_INTERVAL_S = 5;

struct NoiseState {
    unsigned long total = 0;
    unsigned long suppressed = 0;
    time_t lastPrint = 0;
};

std::mutex gNoiseMutex;
NoiseState gNoiseState;

bool IsMidGopJoinNoise(const char* msg)
{
    static const char* prefixes[] = {
        "non-existing PPS",
        "non-existing SPS",
        "decode_slice_header error",
        "no frame!",
        "Error splitting the input into NAL units",
        "Could not find ref with POC",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (strncmp(msg, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

bool AllowNoiseLog(char* suffix, size_t suffixSize)
{
    std::lock_guard<std::mutex> lock(gNoiseMutex);
    gNoiseState.total++;
    time_t now = time(nullptr);
    if (gNoiseState.total > 1 && now - gNoiseState.lastPrint < NOISE_LOG_INTERVAL_S) {
        gNoiseState.suppressed++;
        return false;
    }
    gNoiseState.lastPrint = now;
    if (gNoiseState.suppressed > 0) {
        snprintf(suffix, suffixSize, " (mid-GOP join noise, suppressed x%lu)", gNoiseState.suppressed);
        gNoiseState.suppressed = 0;
    }
    return true;
}

void FfLogCallback(void* ptr, int level, const char* fmt, va_list vl)
{
    if (level > av_log_get_level()) {
        return;
    }
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    LogLevel hl = level <= AV_LOG_ERROR ? LOG_ERROR : (level <= AV_LOG_WARNING ? LOG_WARN : LOG_INFO);
    if (level <= AV_LOG_ERROR && IsMidGopJoinNoise(buf)) {
        char suffix[64] = { 0 };
        if (!AllowNoiseLog(suffix, sizeof(suffix))) {
            return;
        }
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFmpeg", "%{public}s%{public}s", buf, suffix);
        return;
    }
    OH_LOG_Print(LOG_APP, hl, 0xFF00, "FFmpeg", "%{public}s", buf);
}
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    av_log_set_callback(FfLogCallback);

    napi_property_descriptor desc[] = {
        { "probe", nullptr, ffmpeg::Probe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "extractFrame", nullptr, ffmpeg::ExtractFrame, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "transcode", nullptr, ffmpeg::Transcode, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "registerStreamIO", nullptr, ffmpeg::FFRegisterStreamIO, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pushStreamIO", nullptr, ffmpeg::FFPushStreamIO, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "closeStreamIO", nullptr, ffmpeg::FFCloseStreamIO, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    napi_property_descriptor clsDesc[] = {
        { "open", nullptr, ffmpeg::FFAudioDecoderOpen, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "on", nullptr, ffmpeg::FFAudioDecoderOn, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "off", nullptr, ffmpeg::FFAudioDecoderOff, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start", nullptr, ffmpeg::FFAudioDecoderStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "switchTrack", nullptr, ffmpeg::FFAudioDecoderSwitchTrack, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pause", nullptr, ffmpeg::FFAudioDecoderPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resume", nullptr, ffmpeg::FFAudioDecoderResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, ffmpeg::FFAudioDecoderStop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "close", nullptr, ffmpeg::FFAudioDecoderClose, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "flushBacklog", nullptr, ffmpeg::FFAudioDecoderFlushBacklog, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRate", nullptr, ffmpeg::FFAudioDecoderSetRate, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setSpeed", nullptr, ffmpeg::FFAudioDecoderSetSpeed, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "detach", nullptr, ffmpeg::FFAudioDecoderDetach, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getBufferStats", nullptr, ffmpeg::FFAudioDecoderGetBufferStats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getStats", nullptr, ffmpeg::FFAudioDecoderGetStats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "seekTo", nullptr, ffmpeg::FFAudioDecoderSeekTo, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_value cls = nullptr;
    napi_define_class(env, "FFAudioDecoder", NAPI_AUTO_LENGTH, ffmpeg::FFAudioDecoderConstructor,
        nullptr, sizeof(clsDesc) / sizeof(clsDesc[0]), clsDesc, &cls);
    napi_set_named_property(env, exports, "FFAudioDecoder", cls);

    napi_property_descriptor vclsDesc[] = {
        { "open", nullptr, ffmpeg::FFVideoDecoderOpen, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "on", nullptr, ffmpeg::FFVideoDecoderOn, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "off", nullptr, ffmpeg::FFVideoDecoderOff, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start", nullptr, ffmpeg::FFVideoDecoderStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pause", nullptr, ffmpeg::FFVideoDecoderPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resume", nullptr, ffmpeg::FFVideoDecoderResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, ffmpeg::FFVideoDecoderStop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "close", nullptr, ffmpeg::FFVideoDecoderClose, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "flushToKeyframe", nullptr, ffmpeg::FFVideoDecoderFlushToKeyframe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setSpeed", nullptr, ffmpeg::FFVideoDecoderSetSpeed, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "detach", nullptr, ffmpeg::FFVideoDecoderDetach, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getBufferStats", nullptr, ffmpeg::FFVideoDecoderGetBufferStats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getStats", nullptr, ffmpeg::FFVideoDecoderGetStats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "seekTo", nullptr, ffmpeg::FFVideoDecoderSeekTo, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "captureFrame", nullptr, ffmpeg::FFVideoDecoderCaptureFrame, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_value vcls = nullptr;
    napi_define_class(env, "FFVideoDecoder", NAPI_AUTO_LENGTH, ffmpeg::FFVideoDecoderConstructor,
        nullptr, sizeof(vclsDesc) / sizeof(vclsDesc[0]), vclsDesc, &vcls);
    napi_set_named_property(env, exports, "FFVideoDecoder", vcls);

    napi_property_descriptor sclsDesc[] = {
        { "open", nullptr, ffmpeg::FFSubtitleDecoderOpen, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "on", nullptr, ffmpeg::FFSubtitleDecoderOn, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "off", nullptr, ffmpeg::FFSubtitleDecoderOff, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start", nullptr, ffmpeg::FFSubtitleDecoderStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pause", nullptr, ffmpeg::FFSubtitleDecoderPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resume", nullptr, ffmpeg::FFSubtitleDecoderResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, ffmpeg::FFSubtitleDecoderStop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "close", nullptr, ffmpeg::FFSubtitleDecoderClose, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "detach", nullptr, ffmpeg::FFSubtitleDecoderDetach, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRawPump", nullptr, ffmpeg::FFSubtitleDecoderSetRawPump, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setSpeed", nullptr, ffmpeg::FFSubtitleDecoderSetSpeed, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setDelayMs", nullptr, ffmpeg::FFSubtitleDecoderSetDelayMs, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_value scls = nullptr;
    napi_define_class(env, "FFSubtitleDecoder", NAPI_AUTO_LENGTH, ffmpeg::FFSubtitleDecoderConstructor,
        nullptr, sizeof(sclsDesc) / sizeof(sclsDesc[0]), sclsDesc, &scls);
    napi_set_named_property(env, exports, "FFSubtitleDecoder", scls);
    return exports;
}
EXTERN_C_END

NAPI_MODULE(ffmpeg, Init)
