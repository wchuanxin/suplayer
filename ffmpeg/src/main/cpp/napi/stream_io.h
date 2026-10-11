#ifndef FFMPEG_NAPI_STREAM_IO_H
#define FFMPEG_NAPI_STREAM_IO_H

#include <cstdint>
#include <string>

#include "napi/native_api.h"

struct AVFormatContext;
struct AVIOContext;

namespace ffmpeg {

// Custom AVIO: sources registered from JS are addressed as "streamio:<id>"
// URLs. All demux entry points accept such URLs and route every read/seek
// through the registered JS callbacks.

bool IsStreamIOUrl(const std::string& url);

// If `url` is a "streamio:<id>" URL with a registered live source, create an
// AVIOContext backed by the JS callbacks and attach it to `fmt` (pb +
// AVFMT_FLAG_CUSTOM_IO). Returns false with `err` set on failure; returns
// false without touching `err` when `url` is not a streamio URL.
bool AttachStreamIO(AVFormatContext* fmt, const std::string& url, std::string& err);

// Frees an AVIOContext created by AttachStreamIO (no-op for anything else,
// including AVIOContexts libavformat owns). Idempotent.
void ReleaseStreamIO(AVIOContext* pb);

// RAII: captures fmt->pb at construction and releases it on scope exit.
// Construct AFTER AttachStreamIO; the scope must enclose both
// avformat_open_input and avformat_close_input so failure and success paths
// both free the custom AVIOContext.
class StreamIOGuard {
public:
    explicit StreamIOGuard(AVFormatContext* fmt);
    ~StreamIOGuard();
    StreamIOGuard(const StreamIOGuard&) = delete;
    StreamIOGuard& operator=(const StreamIOGuard&) = delete;

private:
    AVIOContext* pb_ = nullptr;
};

} // namespace ffmpeg

namespace ffmpeg {

// ---- NAPI surface ----
// registerStreamIO(id, { read(seq, offset, length), seek?(seq, offset,
// whence), close?() }): register a JS-backed byte source addressable as
// "streamio:<id>". read/seek must complete by calling pushStreamIO with the
// same seq (15 s timeout, a stall fails the source permanently).
napi_value FFRegisterStreamIO(napi_env env, napi_callback_info info);
// pushStreamIO(id, seq, value, data?): deliver a read/seek result.
// read: value = byte count (0 = EOF, < 0 = error), data = ArrayBuffer.
// seek: value = new absolute offset (whence 0/3) or total size (whence 2),
// < 0 = error. Returns whether the response was still awaited.
napi_value FFPushStreamIO(napi_env env, napi_callback_info info);
// closeStreamIO(id): fail all pending/ future IO and release the callbacks.
napi_value FFCloseStreamIO(napi_env env, napi_callback_info info);

} // namespace ffmpeg

#endif
