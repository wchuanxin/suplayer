#ifndef FFMPEG_FRAME_EXTRACTOR_H
#define FFMPEG_FRAME_EXTRACTOR_H

#include "napi/native_api.h"

namespace ffmpeg {

napi_value ExtractFrame(napi_env env, napi_callback_info info);

} // namespace ffmpeg

#endif // FFMPEG_FRAME_EXTRACTOR_H
