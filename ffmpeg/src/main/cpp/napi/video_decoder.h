#ifndef FFMPEG_VIDEO_DECODER_H
#define FFMPEG_VIDEO_DECODER_H

#include "napi/native_api.h"

namespace ffmpeg {

napi_value FFVideoDecoderConstructor(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderOpen(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderOn(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderStart(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderPause(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderResume(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderStop(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderClose(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderFlushToKeyframe(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderDetach(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderGetBufferStats(napi_env env, napi_callback_info info);
napi_value FFVideoDecoderGetStats(napi_env env, napi_callback_info info);

} // namespace ffmpeg

#endif // FFMPEG_VIDEO_DECODER_H
