#ifndef FFMPEG_NAPI_NET_OPTIONS_H
#define FFMPEG_NAPI_NET_OPTIONS_H

#include "napi/native_api.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <cstdint>
#include <cctype>
#include <string>
#include <vector>

namespace ffmpeg {

enum class HwMode {
    AUTO = 0,  // try hardware first, fall back to software on failure
    FORCE = 1, // hardware only, report error on failure
    NEVER = 2, // software only
};

struct NetOptions {
    std::string userAgent;
    std::string headers;
    std::string allowedExtensions;
    int32_t liveStartIndex = INT32_MIN; // INT32_MIN = not set
    int32_t analyzeMs = 0;              // 0 = ffmpeg default
    HwMode hwMode = HwMode::AUTO;       // video decoder backend selection
    int32_t program = INT32_MIN;        // MPEG-TS program_num to lock to; INT32_MIN = not set
};

// Parse optional JS options object into NetOptions.
// Returns false when a present field has wrong type.
inline bool ParseNetOptions(napi_env env, napi_value val, NetOptions& out)
{
    if (val == nullptr) {
        return true;
    }
    napi_valuetype t = napi_undefined;
    napi_typeof(env, val, &t);
    if (t == napi_undefined || t == napi_null) {
        return true;
    }
    if (t != napi_object) {
        return false;
    }

    auto readString = [&](const char* name, std::string& dst) -> bool {
        napi_value v = nullptr;
        if (napi_get_named_property(env, val, name, &v) != napi_ok) {
            return true;
        }
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, v, &vt);
        if (vt == napi_undefined || vt == napi_null) {
            return true;
        }
        if (vt != napi_string) {
            return false;
        }
        size_t len = 0;
        if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) {
            return false;
        }
        std::vector<char> buf(len + 1, '\0');
        napi_get_value_string_utf8(env, v, buf.data(), buf.size(), &len);
        dst.assign(buf.data(), len);
        return true;
    };

    auto readInt = [&](const char* name, int32_t& dst) -> bool {
        napi_value v = nullptr;
        if (napi_get_named_property(env, val, name, &v) != napi_ok) {
            return true;
        }
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, v, &vt);
        if (vt == napi_undefined || vt == napi_null) {
            return true;
        }
        if (vt != napi_number) {
            return false;
        }
        double d = 0.0;
        if (napi_get_value_double(env, v, &d) != napi_ok) {
            return false;
        }
        dst = static_cast<int32_t>(d);
        return true;
    };

    auto readHwMode = [&](const char* name, HwMode& dst) -> bool {
        napi_value v = nullptr;
        if (napi_get_named_property(env, val, name, &v) != napi_ok) {
            return true;
        }
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, v, &vt);
        if (vt == napi_undefined || vt == napi_null) {
            return true;
        }
        if (vt != napi_string) {
            return false;
        }
        size_t len = 0;
        if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) {
            return false;
        }
        std::vector<char> buf(len + 1, '\0');
        napi_get_value_string_utf8(env, v, buf.data(), buf.size(), &len);
        std::string s(buf.data(), len);
        for (char& c : s) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        if (s == "auto") {
            dst = HwMode::AUTO;
        } else if (s == "force") {
            dst = HwMode::FORCE;
        } else if (s == "never") {
            dst = HwMode::NEVER;
        } else {
            return false;
        }
        return true;
    };

    if (!readString("userAgent", out.userAgent) || !readString("headers", out.headers) ||
        !readString("allowedExtensions", out.allowedExtensions) ||
        !readInt("liveStartIndex", out.liveStartIndex) || !readInt("analyzeMs", out.analyzeMs) ||
        !readHwMode("hw", out.hwMode) || !readInt("program", out.program)) {
        return false;
    }
    return true;
}

// Fill open dictionary (user_agent/headers/live_start_index). `dict` is freed by
// caller. `fmt` may be nullptr; when non-null and analyzeMs > 0, analysis duration
// is capped (must be called on an allocated context before avformat_open_input).
inline void ApplyOpenOptions(AVFormatContext* fmt, AVDictionary** dict, const NetOptions& opts)
{
    if (!opts.userAgent.empty()) {
        av_dict_set(dict, "user_agent", opts.userAgent.c_str(), 0);
    }
    if (!opts.headers.empty()) {
        av_dict_set(dict, "headers", opts.headers.c_str(), 0);
    }
    if (!opts.allowedExtensions.empty()) {
        // user takes over extension control: also disable the demuxer-side
        // strict extension/format matching (hls "extension_picky")
        av_dict_set(dict, "allowed_extensions", opts.allowedExtensions.c_str(), 0);
        av_dict_set(dict, "extension_picky", "0", 0);
    }
    if (opts.liveStartIndex != INT32_MIN) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", opts.liveStartIndex);
        av_dict_set(dict, "live_start_index", buf, 0);
    }
    if (fmt != nullptr && opts.analyzeMs > 0) {
        // AV_TIME_BASE is microseconds
        fmt->max_analyze_duration = static_cast<int64_t>(opts.analyzeMs) * 1000;
    }
}

// Lock an opened context to a single MPEG-TS program: returns the AVProgram
// matching programNum (or nullptr when the context carries no such program)
// and marks every stream outside the program AVDISCARD_ALL so av_read_frame
// only yields packets of the selected program.
inline AVProgram* SelectProgram(AVFormatContext* fmt, int32_t programNum)
{
    if (fmt == nullptr || programNum == INT32_MIN) {
        return nullptr;
    }
    for (unsigned int p = 0; p < fmt->nb_programs; ++p) {
        AVProgram* prog = fmt->programs[p];
        if (prog->program_num != programNum) {
            continue;
        }
        for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
            bool inProgram = false;
            for (unsigned int k = 0; k < prog->nb_stream_indexes; ++k) {
                if (prog->stream_index[k] == i) {
                    inProgram = true;
                    break;
                }
            }
            if (!inProgram) {
                fmt->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        return prog;
    }
    return nullptr;
}

// Whether streamIndex belongs to the program (true when no program restriction).
inline bool StreamInProgram(const AVProgram* prog, int streamIndex)
{
    if (prog == nullptr) {
        return true;
    }
    for (unsigned int k = 0; k < prog->nb_stream_indexes; ++k) {
        if (static_cast<int>(prog->stream_index[k]) == streamIndex) {
            return true;
        }
    }
    return false;
}

// First stream of the given media type inside the program (-1 when none).
inline int FindStreamInProgram(const AVFormatContext* fmt, const AVProgram* prog, enum AVMediaType type)
{
    if (fmt == nullptr || prog == nullptr) {
        return -1;
    }
    for (unsigned int k = 0; k < prog->nb_stream_indexes; ++k) {
        unsigned int si = prog->stream_index[k];
        if (si < fmt->nb_streams && fmt->streams[si]->codecpar->codec_type == type) {
            return static_cast<int>(si);
        }
    }
    return -1;
}

} // namespace ffmpeg

#endif
