#ifndef FFMPEG_NAPI_TRANSCODER_H
#define FFMPEG_NAPI_TRANSCODER_H

#include "napi/native_api.h"

namespace ffmpeg {
napi_value Transcode(napi_env env, napi_callback_info info);
}

#endif
