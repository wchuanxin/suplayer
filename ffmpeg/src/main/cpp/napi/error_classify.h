#ifndef FFMPEG_ERROR_CLASSIFY_H
#define FFMPEG_ERROR_CLASSIFY_H

#include <string>

#include "napi/native_api.h"

namespace ffmpeg {

// Best-effort machine-readable category for a decoder runtime error message.
// Mirrors the FFErrorCode TS union ('surface' | 'hw' | 'seek' | 'network' |
// 'open' | 'demux' | 'decode' | 'internal'); unknown shapes fall back to
// 'internal'. Rules are matched in order and the first keyword found in the
// message wins, so root-cause substrings (e.g. the network error inside a
// "shared demux open failed: ..." wrapper) take precedence over the wrapper.
const char* ClassifyErrorMessage(const std::string& msg);

// Invoke an 'error' listener as (message, { code, message }): the classic
// string argument is preserved for pre-1.15 callers; the structured payload
// is additive. Shared by the three TsfnDispatchers. JS thread only.
void CallErrorCallback(napi_env env, napi_value undefined, napi_value cb, const std::string& msg);

} // namespace ffmpeg

#endif // FFMPEG_ERROR_CLASSIFY_H
