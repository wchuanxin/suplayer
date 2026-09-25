#ifndef FFMPEG_SUBTITLE_DECODER_H
#define FFMPEG_SUBTITLE_DECODER_H

#include "napi/native_api.h"

namespace ffmpeg {

napi_value FFSubtitleDecoderConstructor(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderOpen(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderOn(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderStart(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderPause(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderResume(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderStop(napi_env env, napi_callback_info info);
napi_value FFSubtitleDecoderClose(napi_env env, napi_callback_info info);

} // namespace ffmpeg

#endif // FFMPEG_SUBTITLE_DECODER_H
