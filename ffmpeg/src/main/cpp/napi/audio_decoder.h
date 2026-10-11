#ifndef FFMPEG_AUDIO_DECODER_H
#define FFMPEG_AUDIO_DECODER_H

#include "napi/native_api.h"

namespace ffmpeg {

napi_value FFAudioDecoderConstructor(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderOpen(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderOn(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderStart(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderSwitchTrack(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderPause(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderResume(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderStop(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderClose(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderFlushBacklog(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderSetRate(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderDetach(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderGetBufferStats(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderGetStats(napi_env env, napi_callback_info info);
napi_value FFAudioDecoderSeekTo(napi_env env, napi_callback_info info);

} // namespace ffmpeg

#endif // FFMPEG_AUDIO_DECODER_H
