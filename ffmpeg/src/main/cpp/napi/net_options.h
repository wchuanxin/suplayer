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
    bool httpPersistent = true;         // reuse one HTTP connection across HLS segments
    bool reconnect = true;              // auto-retry transient network/HTTP failures
    bool audioAllTracks = true;         // decode every audio track in parallel (false = only the
                                        // selected one; others open lazily on switchTrack)
    int32_t frameEventMs = 0;           // min interval between 'frame' events in ms; 0 = every frame
    int64_t startTimeMs = 0;            // VOD start position in ms: seek to the keyframe at/before
                                        // the target and drop pre-target frames (0 = from head);
                                        // ignored when the source is not seekable (live)
    bool sharedDemux = false;           // B1: route this decoder through the process-wide shared
                                        // demux session so every decoder of the same source shares
                                        // ONE network connection (private connection when false)
    bool live = false;                  // Source is a live stream. Live video subscribers use a
                                        // catch-up overflow policy (drop backlog + resume at the
                                        // next keyframe) instead of blocking the shared demux
                                        // thread, so a slow video consumer can never starve the
                                        // audio subscribers or push the HLS reader off the edge.
    int32_t variantIndex = INT32_MIN;   // A2: HLS master-playlist variant to open (0-based index
                                        // into EXT-X-STREAM-INF order); INT32_MIN = not set
    int64_t maxBandwidth = 0;           // A2: pick the highest-bandwidth variant <= this (bits
                                        // per second); 0 = no bandwidth-based selection
    int64_t targetLatencyMs = 0;        // F1: target live latency; derives live_start_index from
                                        // the playlist segment duration and arms runtime
                                        // catch-up (drop backlog when buffered latency exceeds
                                        // the target + tolerance); 0 = disabled
    int32_t autoReconnect = -1;         // A3: rebuild the session in place on HTTP 401/403/404/
                                        // 429 (re-fetch playlist for a fresh token). -1 = auto
                                        // (live: on, VOD: off), 0 = off, 1 = on
    int32_t reconnectMaxRetries = 3;    // A3/A4: rebuild attempts before giving up (EOF)
    bool dvrSeek = false;               // F2 (experimental): allow seekTo on live DVR windows;
                                        // success still depends on the hls demuxer keeping the
                                        // target segment inside its sliding window
    std::string rtspTransport;          // RTSP/RTP transport: "tcp" | "udp" | "udp_multicast" |
                                        // "http" | "https"; empty = FFmpeg default (udp probe ->
                                        // tcp fallback). Also applied to sdp/rtmp-adjacent inputs
                                        // that consume rtsp_transport (ignored elsewhere).
    int32_t bufferSize = 0;             // Protocol receive/send buffer size in bytes
                                        // (udp/tcp/http/rtsp); 0 = FFmpeg default.
    int64_t rwTimeoutMs = 0;            // I/O read/write timeout in ms: a single read or write
                                        // blocking longer than this aborts with ETIMEDOUT.
                                        // 0 = no timeout (FFmpeg default). Applied to all
                                        // protocol-based sources.
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

    auto readInt64 = [&](const char* name, int64_t& dst) -> bool {
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
        dst = static_cast<int64_t>(d);
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

    auto readBool = [&](const char* name, bool& dst) -> bool {
        napi_value v = nullptr;
        if (napi_get_named_property(env, val, name, &v) != napi_ok) {
            return true;
        }
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, v, &vt);
        if (vt == napi_undefined || vt == napi_null) {
            return true;
        }
        if (vt != napi_boolean) {
            return false;
        }
        bool b = false;
        if (napi_get_value_bool(env, v, &b) != napi_ok) {
            return false;
        }
        dst = b;
        return true;
    };

    auto readTri = [&](const char* name, int32_t& dst) -> bool {
        napi_value v = nullptr;
        if (napi_get_named_property(env, val, name, &v) != napi_ok) {
            return true;
        }
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, v, &vt);
        if (vt == napi_undefined || vt == napi_null) {
            return true;
        }
        if (vt != napi_boolean) {
            return false;
        }
        bool b = false;
        if (napi_get_value_bool(env, v, &b) != napi_ok) {
            return false;
        }
        dst = b ? 1 : 0;
        return true;
    };

    auto readAudioTracks = [&](const char* name, bool& dst) -> bool {        napi_value v = nullptr;
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
        if (s == "all") {
            dst = true;
        } else if (s == "first") {
            dst = false;
        } else {
            return false;
        }
        return true;
    };

    if (!readString("userAgent", out.userAgent) || !readString("headers", out.headers) ||
        !readString("allowedExtensions", out.allowedExtensions) ||
        !readInt("liveStartIndex", out.liveStartIndex) || !readInt("analyzeMs", out.analyzeMs) ||
        !readHwMode("hw", out.hwMode) || !readInt("program", out.program) ||
        !readBool("httpPersistent", out.httpPersistent) || !readBool("reconnect", out.reconnect) ||
        !readAudioTracks("audioTracks", out.audioAllTracks) || !readInt("frameEventMs", out.frameEventMs) ||
        !readInt64("startTimeMs", out.startTimeMs) || !readBool("sharedDemux", out.sharedDemux) ||
        !readBool("live", out.live) || !readInt("variantIndex", out.variantIndex) ||
        !readInt64("maxBandwidth", out.maxBandwidth) || !readInt64("targetLatencyMs", out.targetLatencyMs) ||
        !readTri("autoReconnect", out.autoReconnect) ||
        !readInt("reconnectMaxRetries", out.reconnectMaxRetries) || !readBool("dvrSeek", out.dvrSeek) ||
        !readString("rtspTransport", out.rtspTransport) || !readInt("bufferSize", out.bufferSize) ||
        !readInt64("rwTimeoutMs", out.rwTimeoutMs)) {
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
    // Reuse a single HTTP connection across HLS playlist reloads and segment
    // downloads instead of paying a fresh TCP+TLS handshake per segment (hls
    // demuxer option; ignored by other formats).
    if (opts.httpPersistent) {
        av_dict_set(dict, "http_persistent", "1", 0);
    }
    // Auto-retry transient network failures: connection errors during setup,
    // disconnects on live (non-seekable) streams and HTTP 4xx/5xx segment/
    // playlist responses (CDN edge miss, origin hiccup) are retried with a
    // capped backoff before the error surfaces (http protocol options; no-op
    // for non-http sources).
    if (opts.reconnect) {
        av_dict_set(dict, "reconnect", "1", 0);
        av_dict_set(dict, "reconnect_streamed", "1", 0);
        av_dict_set(dict, "reconnect_on_network_error", "1", 0);
        av_dict_set(dict, "reconnect_on_http_error", "4xx,5xx", 0);
        av_dict_set(dict, "reconnect_delay_max", "5", 0);
    }
    // Non-HLS protocol tuning (rtsp/rtp/udp/tcp/http/srt/...). FFmpeg ignores
    // dict entries a protocol does not consume, so applying them is harmless
    // for other sources.
    if (!opts.rtspTransport.empty()) {
        av_dict_set(dict, "rtsp_transport", opts.rtspTransport.c_str(), 0);
    }
    if (opts.bufferSize > 0) {
        av_dict_set(dict, "buffer_size", std::to_string(opts.bufferSize).c_str(), 0);
    }
    if (opts.rwTimeoutMs > 0) {
        // rw_timeout is consumed by ffurl in microseconds
        av_dict_set(dict, "rw_timeout", std::to_string(opts.rwTimeoutMs * 1000).c_str(), 0);
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
