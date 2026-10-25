#include "error_classify.h"

namespace ffmpeg {
namespace {

struct Rule {
    const char* code;
    const char* keywords[14];
};

// Ordered rules: the first rule with any keyword contained in the message
// wins. Keywords cover the kernel's own emit sites (EmitError strings and
// lastError_ values) plus the FFmpeg/HTTP error fragments they embed.
const Rule kRules[] = {
    { "surface", { "surface wedged", "request buffer", "buffer queue", "NativeWindow", "native window",
                   "SET_BUFFER_GEOMETRY", "window", nullptr } },
    { "hw", { "hw decoder", "OH_VideoDecoder", "OH_AVCodec", "codec service", "codec capability",
              "hardware decode", "no hardware decoder", nullptr } },
    { "seek", { "seekTo", "seek failed", "seek to ", nullptr } },
    { "network", { "HTTP error", "Connection", "connection", "socket", "TLS", "timed out", "timeout",
                   "Server returned", "getaddrinfo" } },
    { "open", { "avformat_open_input", "find_stream_info", "no audio stream", "no video stream",
                "no subtitle stream", "stream index out of range", "does not belong to program",
                "program not found", "open failed", "not opened", "unsupported video codec",
                "avcodec_open2" } },
    { "demux", { "demux", "av_read_frame", nullptr } },
    { "decode", { "avcodec", "swr_convert", "sws_", "atempo", "switchTrack", "decode", "bsf ", nullptr } },
};

} // namespace

const char* ClassifyErrorMessage(const std::string& msg)
{
    if (!msg.empty()) {
        for (const auto& rule : kRules) {
            for (const char* kw : rule.keywords) {
                if (kw == nullptr) {
                    break;
                }
                if (msg.find(kw) != std::string::npos) {
                    return rule.code;
                }
            }
        }
    }
    return "internal";
}

void CallErrorCallback(napi_env env, napi_value undefined, napi_value cb, const std::string& msg)
{
    napi_value msgVal = nullptr;
    napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &msgVal);
    napi_value errObj = nullptr;
    napi_create_object(env, &errObj);
    napi_value codeVal = nullptr;
    napi_create_string_utf8(env, ClassifyErrorMessage(msg), NAPI_AUTO_LENGTH, &codeVal);
    napi_set_named_property(env, errObj, "code", codeVal);
    napi_value messageVal = nullptr;
    napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &messageVal);
    napi_set_named_property(env, errObj, "message", messageVal);
    napi_value cbArgs[2] = { msgVal, errObj };
    napi_call_function(env, undefined, cb, 2, cbArgs, &undefined);
}

} // namespace ffmpeg
