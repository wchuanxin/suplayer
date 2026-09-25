#include "napi/native_api.h"
#include "probe.h"
#include "frame_extractor.h"
#include "audio_decoder.h"
#include "video_decoder.h"
#include "subtitle_decoder.h"
#include "transcoder.h"

extern "C" {
#include <libavutil/log.h>
#include <hilog/log.h>
}

#include <cstdarg>
#include <cstdio>

namespace {
void FfLogCallback(void* ptr, int level, const char* fmt, va_list vl)
{
    if (level > av_log_get_level()) {
        return;
    }
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    LogLevel hl = level <= AV_LOG_ERROR ? LOG_ERROR : (level <= AV_LOG_WARNING ? LOG_WARN : LOG_INFO);
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
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    napi_property_descriptor clsDesc[] = {
        { "open", nullptr, ffmpeg::FFAudioDecoderOpen, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "on", nullptr, ffmpeg::FFAudioDecoderOn, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start", nullptr, ffmpeg::FFAudioDecoderStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "switchTrack", nullptr, ffmpeg::FFAudioDecoderSwitchTrack, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pause", nullptr, ffmpeg::FFAudioDecoderPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resume", nullptr, ffmpeg::FFAudioDecoderResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, ffmpeg::FFAudioDecoderStop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "close", nullptr, ffmpeg::FFAudioDecoderClose, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_value cls = nullptr;
    napi_define_class(env, "FFAudioDecoder", NAPI_AUTO_LENGTH, ffmpeg::FFAudioDecoderConstructor,
        nullptr, sizeof(clsDesc) / sizeof(clsDesc[0]), clsDesc, &cls);
    napi_set_named_property(env, exports, "FFAudioDecoder", cls);

    napi_property_descriptor vclsDesc[] = {
        { "open", nullptr, ffmpeg::FFVideoDecoderOpen, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "on", nullptr, ffmpeg::FFVideoDecoderOn, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start", nullptr, ffmpeg::FFVideoDecoderStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pause", nullptr, ffmpeg::FFVideoDecoderPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resume", nullptr, ffmpeg::FFVideoDecoderResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, ffmpeg::FFVideoDecoderStop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "close", nullptr, ffmpeg::FFVideoDecoderClose, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_value vcls = nullptr;
    napi_define_class(env, "FFVideoDecoder", NAPI_AUTO_LENGTH, ffmpeg::FFVideoDecoderConstructor,
        nullptr, sizeof(vclsDesc) / sizeof(vclsDesc[0]), vclsDesc, &vcls);
    napi_set_named_property(env, exports, "FFVideoDecoder", vcls);

    napi_property_descriptor sclsDesc[] = {
        { "open", nullptr, ffmpeg::FFSubtitleDecoderOpen, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "on", nullptr, ffmpeg::FFSubtitleDecoderOn, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start", nullptr, ffmpeg::FFSubtitleDecoderStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pause", nullptr, ffmpeg::FFSubtitleDecoderPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resume", nullptr, ffmpeg::FFSubtitleDecoderResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, ffmpeg::FFSubtitleDecoderStop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "close", nullptr, ffmpeg::FFSubtitleDecoderClose, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_value scls = nullptr;
    napi_define_class(env, "FFSubtitleDecoder", NAPI_AUTO_LENGTH, ffmpeg::FFSubtitleDecoderConstructor,
        nullptr, sizeof(sclsDesc) / sizeof(sclsDesc[0]), sclsDesc, &scls);
    napi_set_named_property(env, exports, "FFSubtitleDecoder", scls);
    return exports;
}
EXTERN_C_END

NAPI_MODULE(ffmpeg, Init)
