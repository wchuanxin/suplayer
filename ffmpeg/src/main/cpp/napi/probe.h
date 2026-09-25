#ifndef FFMPEG_PROBE_H
#define FFMPEG_PROBE_H

#include "napi/native_api.h"

namespace ffmpeg {

napi_value Probe(napi_env env, napi_callback_info info);

} // namespace ffmpeg

#endif // FFMPEG_PROBE_H
