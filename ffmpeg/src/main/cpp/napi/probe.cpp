#include "probe.h"
#include "net_options.h"
#include "demux_session.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/codec_desc.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
}

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ffmpeg {
namespace {

struct StreamResult {
    int32_t index = 0;
    std::string type;
    std::string codec;
    std::string codecLongName;
    int32_t width = 0;
    int32_t height = 0;
    int32_t sampleRate = 0;
    int32_t channels = 0;
    std::string language;
    std::string title;
    double duration = 0.0;
    int64_t bitRate = 0;
    int32_t pid = -1;        // MPEG-TS packet id (AVStream::id), -1 when unknown
    int32_t programIndex = -1; // index into ProbeContext::programs, -1 when not in any program
};

struct ProgramResult {
    int32_t index = 0;
    int32_t programNum = 0;
    int32_t pmtPid = -1;
    int32_t pcrPid = -1;
    int32_t pmtVersion = -1;
    int32_t flags = 0; // AV_PROGRAM_RUNNING = 1
    // HLS variant bandwidth (BANDWIDTH attribute, bits/s) — the FFmpeg hls
    // demuxer publishes it as the program's "variant_bitrate" metadata; 0
    // when absent (plain mpegts etc.).
    int64_t bandwidth = 0;
    std::string serviceName;
    std::string serviceProvider;
    std::vector<int32_t> streamIndexes;
};

struct ProbeContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string src;
    NetOptions netOpts;
    std::string error;
    std::string format;
    std::string formatLongName;
    std::string title;
    double duration = 0.0;
    int64_t bitRate = 0;
    std::vector<StreamResult> streams;
    std::vector<ProgramResult> programs;
};

std::string ErrToStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

std::string MediaTypeToString(enum AVMediaType type)
{
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:
            return "video";
        case AVMEDIA_TYPE_AUDIO:
            return "audio";
        case AVMEDIA_TYPE_SUBTITLE:
            return "subtitle";
        case AVMEDIA_TYPE_DATA:
            return "data";
        case AVMEDIA_TYPE_ATTACHMENT:
            return "attachment";
        default:
            return "unknown";
    }
}

void SetStringProp(napi_env env, napi_value obj, const char* name, const std::string& value)
{
    napi_value v = nullptr;
    napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v);
    napi_set_named_property(env, obj, name, v);
}

void SetNumberProp(napi_env env, napi_value obj, const char* name, double value)
{
    napi_value v = nullptr;
    napi_create_double(env, value, &v);
    napi_set_named_property(env, obj, name, v);
}

std::string DictGet(const AVDictionary* dict, const char* key)
{
    const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
    return entry == nullptr ? std::string() : std::string(entry->value);
}

void ProbeExecute(napi_env env, void* data)
{
    auto* ctx = static_cast<ProbeContext*>(data);
    // B1 shared demux: when a live session exists for this source + matching
    // demux options, reuse its already-open context for the metadata read —
    // no extra connection, no packet consumption. The shared_ptr keeps the
    // session alive until the metadata is copied.
    std::shared_ptr<DemuxSession> reuse = DemuxSession::Find(ctx->src, ctx->netOpts);
    AVFormatContext* fmt = nullptr;
    if (reuse != nullptr) {
        fmt = reuse->fmt();
    } else {
        AVDictionary* dict = nullptr;
        ApplyOpenOptions(nullptr, &dict, ctx->netOpts);
        int ret = avformat_open_input(&fmt, ctx->src.c_str(), nullptr, &dict);
        av_dict_free(&dict);
        if (ret < 0) {
            ctx->error = "avformat_open_input failed: " + ErrToStr(ret);
            return;
        }
        if (ctx->netOpts.analyzeMs > 0) {
            fmt->max_analyze_duration = static_cast<int64_t>(ctx->netOpts.analyzeMs) * 1000;
        }
        ret = avformat_find_stream_info(fmt, nullptr);
        if (ret < 0) {
            ctx->error = "avformat_find_stream_info failed: " + ErrToStr(ret);
            avformat_close_input(&fmt);
            return;
        }
    }

    ctx->format = fmt->iformat != nullptr && fmt->iformat->name != nullptr ? fmt->iformat->name : "";
    ctx->formatLongName =
        fmt->iformat != nullptr && fmt->iformat->long_name != nullptr ? fmt->iformat->long_name : "";
    ctx->title = DictGet(fmt->metadata, "title");
    ctx->duration = fmt->duration > 0 ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
    ctx->bitRate = fmt->bit_rate;

    // AVStream::id only carries an MPEG-TS PID for the raw mpegts demuxer; for
    // MP4 it is the track id and for HLS the playlist index, so gate on it.
    const bool pidValid = fmt->iformat != nullptr && fmt->iformat->name != nullptr &&
        strcmp(fmt->iformat->name, "mpegts") == 0;

    // Programs (PAT/PMT view, mpegts demuxer; empty for containers without PSI)
    for (unsigned int p = 0; p < fmt->nb_programs; ++p) {
        AVProgram* prog = fmt->programs[p];
        ProgramResult pr;
        pr.index = static_cast<int32_t>(p);
        pr.programNum = prog->program_num;
        // For non-mpegts demuxers (HLS variant programs) these PSI fields carry
        // placeholder zeros, so report them as unknown instead.
        pr.pmtPid = pidValid ? prog->pmt_pid : -1;
        pr.pcrPid = pidValid ? prog->pcr_pid : -1;
        pr.pmtVersion = prog->pmt_version;
        pr.flags = prog->flags;
        pr.serviceName = DictGet(prog->metadata, "service_name");
        pr.serviceProvider = DictGet(prog->metadata, "service_provider");
        // HLS variant BANDWIDTH (A2): expose it so the app can rank/pick
        // variants by real bandwidth instead of guessing from resolution.
        const std::string variantBitrate = DictGet(prog->metadata, "variant_bitrate");
        if (!variantBitrate.empty()) {
            pr.bandwidth = strtoll(variantBitrate.c_str(), nullptr, 10);
        }
        for (unsigned int k = 0; k < prog->nb_stream_indexes; ++k) {
            pr.streamIndexes.push_back(static_cast<int32_t>(prog->stream_index[k]));
        }
        ctx->programs.push_back(pr);
    }

    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        const AVCodecParameters* par = stream->codecpar;
        StreamResult s;
        s.index = static_cast<int32_t>(stream->index);
        s.type = MediaTypeToString(par->codec_type);
        const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
        s.codec = desc != nullptr && desc->name != nullptr ? desc->name : "unknown";
        s.codecLongName = desc != nullptr && desc->long_name != nullptr ? desc->long_name : "";
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            s.width = par->width;
            s.height = par->height;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            s.sampleRate = par->sample_rate;
            s.channels = par->ch_layout.nb_channels;
        }
        s.language = DictGet(stream->metadata, "language");
        s.title = DictGet(stream->metadata, "title");
        s.duration =
            stream->duration > 0 ? static_cast<double>(stream->duration) * av_q2d(stream->time_base) : 0.0;
        s.bitRate = par->bit_rate;
        s.pid = pidValid && stream->id > 0 ? static_cast<int32_t>(stream->id) : -1;
        for (size_t p = 0; p < ctx->programs.size(); ++p) {
            for (int32_t si : ctx->programs[p].streamIndexes) {
                if (si == s.index) {
                    s.programIndex = static_cast<int32_t>(p);
                    break;
                }
            }
            if (s.programIndex >= 0) {
                break;
            }
        }
        ctx->streams.push_back(s);
    }

    if (reuse == nullptr) {
        avformat_close_input(&fmt);
    }
    // `reuse` releases its session reference here; the session itself stays
    // alive while any decoder still holds one.
}

void ProbeComplete(napi_env env, napi_status status, void* data)
{
    auto* ctx = static_cast<ProbeContext*>(data);

    if (!ctx->error.empty()) {
        napi_value code = nullptr;
        napi_value msg = nullptr;
        napi_value err = nullptr;
        napi_create_string_utf8(env, "ffmpeg:probe_error", NAPI_AUTO_LENGTH, &code);
        napi_create_string_utf8(env, ctx->error.c_str(), NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, code, msg, &err);
        napi_reject_deferred(env, ctx->deferred, err);
    } else {
        napi_value result = nullptr;
        napi_create_object(env, &result);
        SetStringProp(env, result, "format", ctx->format);
        SetStringProp(env, result, "formatLongName", ctx->formatLongName);
        SetStringProp(env, result, "title", ctx->title);
        SetNumberProp(env, result, "duration", ctx->duration);
        SetNumberProp(env, result, "bitRate", static_cast<double>(ctx->bitRate));

        napi_value streams = nullptr;
        napi_create_array_with_length(env, ctx->streams.size(), &streams);
        for (size_t i = 0; i < ctx->streams.size(); ++i) {
            const StreamResult& s = ctx->streams[i];
            napi_value obj = nullptr;
            napi_create_object(env, &obj);
            SetNumberProp(env, obj, "index", s.index);
            SetStringProp(env, obj, "type", s.type);
            SetStringProp(env, obj, "codec", s.codec);
            SetStringProp(env, obj, "codecLongName", s.codecLongName);
            SetNumberProp(env, obj, "width", s.width);
            SetNumberProp(env, obj, "height", s.height);
            SetNumberProp(env, obj, "sampleRate", s.sampleRate);
            SetNumberProp(env, obj, "channels", s.channels);
            SetStringProp(env, obj, "language", s.language);
            SetStringProp(env, obj, "title", s.title);
            SetNumberProp(env, obj, "duration", s.duration);
            SetNumberProp(env, obj, "bitRate", static_cast<double>(s.bitRate));
            SetNumberProp(env, obj, "pid", s.pid);
            SetNumberProp(env, obj, "programIndex", s.programIndex);
            napi_set_element(env, streams, static_cast<uint32_t>(i), obj);
        }
        napi_set_named_property(env, result, "streams", streams);

        napi_value programs = nullptr;
        napi_create_array_with_length(env, ctx->programs.size(), &programs);
        for (size_t i = 0; i < ctx->programs.size(); ++i) {
            const ProgramResult& p = ctx->programs[i];
            napi_value obj = nullptr;
            napi_create_object(env, &obj);
            SetNumberProp(env, obj, "index", p.index);
            SetNumberProp(env, obj, "programNum", p.programNum);
            SetNumberProp(env, obj, "pmtPid", p.pmtPid);
            SetNumberProp(env, obj, "pcrPid", p.pcrPid);
            SetNumberProp(env, obj, "pmtVersion", p.pmtVersion);
            SetNumberProp(env, obj, "flags", p.flags);
            SetNumberProp(env, obj, "bandwidth", static_cast<double>(p.bandwidth));
            SetStringProp(env, obj, "serviceName", p.serviceName);
            SetStringProp(env, obj, "serviceProvider", p.serviceProvider);
            napi_value arr = nullptr;
            napi_create_array_with_length(env, p.streamIndexes.size(), &arr);
            for (size_t k = 0; k < p.streamIndexes.size(); ++k) {
                napi_value v = nullptr;
                napi_create_int32(env, p.streamIndexes[k], &v);
                napi_set_element(env, arr, static_cast<uint32_t>(k), v);
            }
            napi_set_named_property(env, obj, "streamIndexes", arr);
            napi_set_element(env, programs, static_cast<uint32_t>(i), obj);
        }
        napi_set_named_property(env, result, "programs", programs);
        napi_resolve_deferred(env, ctx->deferred, result);
    }

    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

} // namespace

napi_value Probe(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "probe(src: string, options?: object) requires 1 argument");
        return nullptr;
    }

    size_t len = 0;
    if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) {
        napi_throw_type_error(env, nullptr, "src must be a string");
        return nullptr;
    }
    std::vector<char> buf(len + 1, '\0');
    napi_get_value_string_utf8(env, args[0], buf.data(), buf.size(), &len);

    auto* ctx = new ProbeContext();
    ctx->src.assign(buf.data(), len);
    if (argc >= 2 && !ParseNetOptions(env, args[1], ctx->netOpts)) {
        delete ctx;
        napi_throw_type_error(env, nullptr,
            "options must be { userAgent?, headers?, allowedExtensions?, liveStartIndex?, analyzeMs?, hw?, program?, httpPersistent?, reconnect?, variantIndex?, maxBandwidth?, targetLatencyMs?, autoReconnect?, reconnectMaxRetries?, dvrSeek? }");
        return nullptr;
    }

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_probe", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ProbeExecute, ProbeComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);

    return promise;
}

} // namespace ffmpeg
