#include "transcoder.h"
#include "stream_io.h"

#include <string>
#include <cstring>

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/opt.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/channel_layout.h"
#include "libavutil/samplefmt.h"
#include "libswresample/swresample.h"
}

namespace ffmpeg {
namespace {

struct TranscodeWork {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string src;
    std::string dst;
    int width = 0;
    int height = 0;
    int fps = 0;
    int64_t videoBitrate = 800000;
    int64_t audioBitrate = 96000;
    int64_t startTimeMs = 0;
    int64_t durationMs = 0;
    std::string vcodec; // "" / "h264" (libopenh264, default) | "mpeg2" | "copy"
    std::string acodec; // "" / "aac" (default) | "ac3" | "copy"
    std::string filter; // custom libavfilter video chain; replaces scale/fps

    int64_t outSize = 0;
    int64_t frames = 0;
    double outDurMs = 0.0;
    std::string err;
};

static std::string ErrToStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

struct VideoPipe {
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    AVFilterGraph* graph = nullptr;
    AVFilterContext* bufSrc = nullptr;
    AVFilterContext* bufSink = nullptr;
    AVStream* inStream = nullptr;
    AVStream* outStream = nullptr;
    AVBSFContext* bsf = nullptr; // copy mode: mp4/mkv -> mpegts Annex-B conversion
    int inIndex = -1;
};

struct AudioPipe {
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVStream* inStream = nullptr;
    AVStream* outStream = nullptr;
    AVBSFContext* bsf = nullptr; // reserved (copy mode bookkeeping)
    int inIndex = -1;
    int64_t nextPts = 0; // in samples at output rate (normalized to 0)
    int64_t firstPtsSamples = AV_NOPTS_VALUE;
};

static bool SendFrameToEncoder(AVCodecContext* enc, AVFrame* frame, AVStream* ost,
    AVFormatContext* ofmt, int64_t& frameCount, double* maxOutMs)
{
    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        return false;
    }
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc, pkt) >= 0) {
        av_packet_rescale_ts(pkt, enc->time_base, ost->time_base);
        pkt->stream_index = ost->index;
        if (maxOutMs != nullptr && pkt->pts != AV_NOPTS_VALUE) {
            double endMs = static_cast<double>(pkt->pts + (pkt->duration > 0 ? pkt->duration : 0)) *
                av_q2d(ost->time_base) * 1000.0;
            if (endMs > *maxOutMs) {
                *maxOutMs = endMs;
            }
        }
        av_interleaved_write_frame(ofmt, pkt);
        av_packet_unref(pkt);
        frameCount++;
    }
    av_packet_free(&pkt);
    return true;
}

// Copy/remux path: write one input packet straight to the output stream,
// rescaling timestamps from the input time base. `bsf` (optional) runs the
// packet through a bitstream filter first (e.g. h264_mp4toannexb). The input
// packet is not consumed.
static bool WriteCopyPacket(AVBSFContext* bsf, const AVPacket* in, AVRational inTb, AVStream* ost,
    AVFormatContext* ofmt, int64_t& frameCount, double* maxOutMs)
{
    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        return false;
    }
    if (bsf != nullptr) {
        if (av_packet_ref(pkt, in) < 0) {
            av_packet_free(&pkt);
            return false;
        }
        if (av_bsf_send_packet(bsf, pkt) < 0) {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            return false;
        }
        av_packet_unref(pkt);
        if (av_bsf_receive_packet(bsf, pkt) < 0) {
            // EAGAIN: filter consumed the packet without emitting one yet
            av_packet_free(&pkt);
            return true;
        }
    } else if (av_packet_ref(pkt, in) < 0) {
        av_packet_free(&pkt);
        return false;
    }
    av_packet_rescale_ts(pkt, inTb, ost->time_base);
    pkt->stream_index = ost->index;
    if (maxOutMs != nullptr && pkt->pts != AV_NOPTS_VALUE) {
        double endMs = static_cast<double>(pkt->pts + (pkt->duration > 0 ? pkt->duration : 0)) *
            av_q2d(ost->time_base) * 1000.0;
        if (endMs > *maxOutMs) {
            *maxOutMs = endMs;
        }
    }
    bool ok = av_interleaved_write_frame(ofmt, pkt) >= 0;
    av_packet_unref(pkt);
    av_packet_free(&pkt);
    if (ok) {
        frameCount++;
    }
    return ok;
}

static bool WriteVideoFrame(VideoPipe& vp, AVFrame* frame, AVFormatContext* ofmt, int64_t& frameCount,
    double* maxOutMs)
{
    int ret = av_buffersrc_add_frame_flags(vp.bufSrc, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        return false;
    }
    AVFrame* oframe = av_frame_alloc();
    while (av_buffersink_get_frame(vp.bufSink, oframe) >= 0) {
        if (!SendFrameToEncoder(vp.enc, oframe, vp.outStream, ofmt, frameCount, maxOutMs)) {
            av_frame_unref(oframe);
            av_frame_free(&oframe);
            return false;
        }
        av_frame_unref(oframe);
    }
    av_frame_free(&oframe);
    return true;
}

static bool FlushVideoPipe(VideoPipe& vp, AVFormatContext* ofmt, int64_t& frameCount, double* maxOutMs)
{
    if (vp.bufSrc != nullptr && vp.bufSink != nullptr) {
        if (av_buffersrc_add_frame_flags(vp.bufSrc, nullptr, 0) < 0) {
            return false; // EOF signal rejected: filter graph in error state
        }
        AVFrame* oframe = av_frame_alloc();
        while (av_buffersink_get_frame(vp.bufSink, oframe) >= 0) {
            if (!SendFrameToEncoder(vp.enc, oframe, vp.outStream, ofmt, frameCount, maxOutMs)) {
                av_frame_unref(oframe);
                av_frame_free(&oframe);
                return false;
            }
            av_frame_unref(oframe);
        }
        av_frame_free(&oframe);
    }
    if (vp.enc != nullptr) {
        return SendFrameToEncoder(vp.enc, nullptr, vp.outStream, ofmt, frameCount, maxOutMs);
    }
    return true;
}

static bool PushAudioSamples(AudioPipe& ap, uint8_t** data, int samples, AVFormatContext* ofmt,
    int64_t& frameCount, int64_t endSample /* -1 = no cutoff */, double* maxOutMs)
{
    if (endSample >= 0 && ap.nextPts + samples > endSample) {
        samples = static_cast<int>(endSample - ap.nextPts);
        if (samples <= 0) {
            return true;
        }
    }
    if (av_audio_fifo_realloc(ap.fifo, av_audio_fifo_size(ap.fifo) + samples) < 0 ||
        av_audio_fifo_write(ap.fifo, reinterpret_cast<void**>(data), samples) < samples) {
        return false;
    }
    const int frameSize = ap.enc->frame_size;
    while (av_audio_fifo_size(ap.fifo) >= frameSize) {
        AVFrame* oframe = av_frame_alloc();
        oframe->format = ap.enc->sample_fmt;
        oframe->sample_rate = ap.enc->sample_rate;
        av_channel_layout_copy(&oframe->ch_layout, &ap.enc->ch_layout);
        oframe->nb_samples = frameSize;
        if (av_frame_get_buffer(oframe, 0) < 0) {
            av_frame_free(&oframe);
            return false;
        }
        if (av_audio_fifo_read(ap.fifo, reinterpret_cast<void**>(oframe->data), frameSize) < frameSize) {
            av_frame_free(&oframe);
            return false;
        }
        oframe->pts = ap.nextPts;
        ap.nextPts += frameSize;
        bool ok = SendFrameToEncoder(ap.enc, oframe, ap.outStream, ofmt, frameCount, maxOutMs);
        av_frame_free(&oframe);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool FlushAudioPipe(AudioPipe& ap, AVFormatContext* ofmt, int64_t& frameCount, double* maxOutMs)
{
    if (ap.enc == nullptr) {
        return true;
    }
    const int frameSize = ap.enc->frame_size;
    if (frameSize <= 0 || av_audio_fifo_size(ap.fifo) == 0) {
        return SendFrameToEncoder(ap.enc, nullptr, ap.outStream, ofmt, frameCount, maxOutMs);
    }
    // pad trailing partial frame with silence
    while (av_audio_fifo_size(ap.fifo) >= frameSize ||
        (av_audio_fifo_size(ap.fifo) > 0 && ap.enc->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) {
        AVFrame* oframe = av_frame_alloc();
        oframe->format = ap.enc->sample_fmt;
        oframe->sample_rate = ap.enc->sample_rate;
        av_channel_layout_copy(&oframe->ch_layout, &ap.enc->ch_layout);
        oframe->nb_samples = FFMIN(frameSize, av_audio_fifo_size(ap.fifo));
        if (av_frame_get_buffer(oframe, 0) < 0) {
            av_frame_free(&oframe);
            return false;
        }
        av_samples_set_silence(oframe->data, 0, oframe->nb_samples, oframe->ch_layout.nb_channels,
            ap.enc->sample_fmt);
        if (av_audio_fifo_read(ap.fifo, reinterpret_cast<void**>(oframe->data), oframe->nb_samples) < 0) {
            av_frame_free(&oframe);
            return false;
        }
        oframe->pts = ap.nextPts;
        ap.nextPts += oframe->nb_samples;
        bool ok = SendFrameToEncoder(ap.enc, oframe, ap.outStream, ofmt, frameCount, maxOutMs);
        av_frame_free(&oframe);
        if (!ok) {
            return false;
        }
        if (av_audio_fifo_size(ap.fifo) == 0) {
            break;
        }
    }
    return SendFrameToEncoder(ap.enc, nullptr, ap.outStream, ofmt, frameCount, maxOutMs);
}

static void RunTranscode(TranscodeWork* w)
{
    AVFormatContext* ifmt = avformat_alloc_context();
    AVIOContext* inIoPb = nullptr; // custom input AVIOContext (streamio sources)
    AVFormatContext* ofmt = nullptr;
    VideoPipe vp;
    AudioPipe ap;
    bool ofmtOpened = false;
    const bool vcopy = w->vcodec == "copy";
    const bool acopy = w->acodec == "copy";

    auto fail = [&](const std::string& msg) {
        w->err = msg;
        if (ifmt != nullptr) {
            AVIOContext* pb = ifmt->pb;
            avformat_close_input(&ifmt);
            ReleaseStreamIO(pb);
        }
        // A failed avformat_open_input frees the context but never a custom
        // AVIOContext; release it here (no-op for libavformat-owned pb).
        ReleaseStreamIO(inIoPb);
        if (ofmt != nullptr) {
            if (ofmtOpened && !(ofmt->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&ofmt->pb);
            }
            avformat_free_context(ofmt);
        }
        if (vp.dec != nullptr) avcodec_free_context(&vp.dec);
        if (vp.enc != nullptr) avcodec_free_context(&vp.enc);
        if (vp.graph != nullptr) avfilter_graph_free(&vp.graph);
        if (vp.bsf != nullptr) av_bsf_free(&vp.bsf);
        if (ap.dec != nullptr) avcodec_free_context(&ap.dec);
        if (ap.enc != nullptr) avcodec_free_context(&ap.enc);
        if (ap.swr != nullptr) swr_free(&ap.swr);
        if (ap.fifo != nullptr) av_audio_fifo_free(ap.fifo);
        if (ap.bsf != nullptr) av_bsf_free(&ap.bsf);
    };

    std::string ioErr;
    if (!AttachStreamIO(ifmt, w->src, ioErr)) {
        if (!ioErr.empty()) {
            fail(ioErr);
            return;
        }
    }
    inIoPb = ifmt->pb;
    int ret = avformat_open_input(&ifmt, w->src.c_str(), nullptr, nullptr);
    if (ret < 0) {
        fail("avformat_open_input failed: " + ErrToStr(ret));
        return;
    }
    if (avformat_find_stream_info(ifmt, nullptr) < 0) {
        fail("avformat_find_stream_info failed");
        return;
    }

    vp.inIndex = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    ap.inIndex = (w->audioBitrate > 0)
        ? av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;

    // ---- video decode (skipped in copy/remux mode) ----
    if (vp.inIndex >= 0 && !vcopy) {
        vp.inStream = ifmt->streams[vp.inIndex];
        const AVCodec* decCodec = avcodec_find_decoder(vp.inStream->codecpar->codec_id);
        if (decCodec == nullptr) {
            fail("video decoder not found");
            return;
        }
        vp.dec = avcodec_alloc_context3(decCodec);
        if (avcodec_parameters_to_context(vp.dec, vp.inStream->codecpar) < 0 ||
            avcodec_open2(vp.dec, decCodec, nullptr) < 0) {
            fail("video decoder open failed");
            return;
        }
    }

    // ---- audio decode (skipped in copy/remux mode) ----
    if (ap.inIndex >= 0 && !acopy) {
        ap.inStream = ifmt->streams[ap.inIndex];
        const AVCodec* decCodec = avcodec_find_decoder(ap.inStream->codecpar->codec_id);
        if (decCodec == nullptr) {
            fail("audio decoder not found");
            return;
        }
        ap.dec = avcodec_alloc_context3(decCodec);
        if (avcodec_parameters_to_context(ap.dec, ap.inStream->codecpar) < 0 ||
            avcodec_open2(ap.dec, decCodec, nullptr) < 0) {
            fail("audio decoder open failed");
            return;
        }
    }

    // ---- output context ----
    ret = avformat_alloc_output_context2(&ofmt, nullptr, nullptr, w->dst.c_str());
    if (ret < 0 || ofmt == nullptr) {
        fail("avformat_alloc_output_context2 failed (unsupported output format)");
        return;
    }

    // ---- video output ----
    if (vp.inIndex >= 0) {
        vp.outStream = avformat_new_stream(ofmt, nullptr);
        if (vp.outStream == nullptr) {
            fail("video output stream alloc failed");
            return;
        }
        if (vcopy) {
            // Remux: no decode/encode, codec parameters copied verbatim.
            avcodec_parameters_copy(vp.outStream->codecpar, vp.inStream->codecpar);
            vp.outStream->codecpar->codec_tag = 0; // let the muxer pick
            vp.outStream->time_base = vp.inStream->time_base;
            // h264/hevc from length-prefixed containers (mp4/mkv) must be
            // converted to Annex-B for MPEG-TS.
            const bool isTs = ofmt->oformat != nullptr && ofmt->oformat->name != nullptr &&
                strstr(ofmt->oformat->name, "mpegts") != nullptr;
            const AVCodecID cid = vp.outStream->codecpar->codec_id;
            if (isTs && (cid == AV_CODEC_ID_H264 || cid == AV_CODEC_ID_HEVC)) {
                const char* bsfName = (cid == AV_CODEC_ID_H264) ? "h264_mp4toannexb" : "hevc_mp4toannexb";
                const AVBitStreamFilter* bsfFilter = av_bsf_get_by_name(bsfName);
                if (bsfFilter == nullptr || av_bsf_alloc(bsfFilter, &vp.bsf) < 0) {
                    fail(std::string("bsf alloc failed: ") + bsfName);
                    return;
                }
                avcodec_parameters_copy(vp.bsf->par_in, vp.inStream->codecpar);
                vp.bsf->time_base_in = vp.inStream->time_base;
                if (av_bsf_init(vp.bsf) < 0) {
                    fail(std::string("bsf init failed: ") + bsfName);
                    return;
                }
            }
        } else {
            const char* encName = (w->vcodec == "mpeg2") ? "mpeg2video" : "libopenh264";
            const AVCodec* encCodec = avcodec_find_encoder_by_name(encName);
            if (encCodec == nullptr) {
                fail(std::string("video encoder not found: ") + encName);
                return;
            }
            vp.graph = avfilter_graph_alloc();
            char args[512];
            AVRational decTb = (vp.dec->time_base.num != 0) ? vp.dec->time_base
                : vp.inStream->time_base;
            AVRational sar = vp.dec->sample_aspect_ratio;
            if (sar.num == 0) {
                sar.num = 1;
                sar.den = 1;
            }
            snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                vp.dec->width, vp.dec->height, vp.dec->pix_fmt,
                decTb.num, decTb.den, sar.num, sar.den);
            const AVFilter* srcFilter = avfilter_get_by_name("buffer");
            const AVFilter* sinkFilter = avfilter_get_by_name("buffersink");
            if (avfilter_graph_create_filter(&vp.bufSrc, srcFilter, "in", args, nullptr, vp.graph) < 0) {
                fail("buffersrc create failed");
                return;
            }
            const enum AVPixelFormat pixFmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
            if (avfilter_graph_create_filter(&vp.bufSink, sinkFilter, "out", nullptr, nullptr, vp.graph) < 0 ||
                av_opt_set_int_list(vp.bufSink, "pix_fmts", pixFmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0) {
                fail("buffersink create failed");
                return;
            }
            // Custom filter chain: replaces the built-in scale/fps so the
            // caller has full control (setpts stays to normalize timestamps).
            std::string filters = "setpts=PTS-STARTPTS";
            if (!w->filter.empty()) {
                filters += ",";
                filters += w->filter;
            } else {
                if (w->width > 0 && w->height > 0) {
                    filters += ",scale=" + std::to_string(w->width) + ":" + std::to_string(w->height);
                }
                if (w->fps > 0) {
                    filters += ",fps=" + std::to_string(w->fps);
                }
            }
            AVFilterInOut* inputs = avfilter_inout_alloc();
            AVFilterInOut* outputs = avfilter_inout_alloc();
            outputs->name = av_strdup("in");
            outputs->filter_ctx = vp.bufSrc;
            outputs->pad_idx = 0;
            outputs->next = nullptr;
            inputs->name = av_strdup("out");
            inputs->filter_ctx = vp.bufSink;
            inputs->pad_idx = 0;
            inputs->next = nullptr;
            ret = avfilter_graph_parse_ptr(vp.graph, filters.c_str(), &inputs, &outputs, nullptr);
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            if (ret < 0 || avfilter_graph_config(vp.graph, nullptr) < 0) {
                fail("filter graph config failed: " + filters);
                return;
            }

            vp.enc = avcodec_alloc_context3(encCodec);
            vp.enc->width = av_buffersink_get_w(vp.bufSink);
            vp.enc->height = av_buffersink_get_h(vp.bufSink);
            vp.enc->pix_fmt = static_cast<AVPixelFormat>(av_buffersink_get_format(vp.bufSink));
            vp.enc->time_base = av_buffersink_get_time_base(vp.bufSink);
            vp.enc->bit_rate = w->videoBitrate;
            vp.enc->gop_size = 30;
            if (avcodec_open2(vp.enc, encCodec, nullptr) < 0) {
                fail(std::string("video encoder open failed: ") + encName);
                return;
            }
            vp.outStream->time_base = vp.enc->time_base;
            if (avcodec_parameters_from_context(vp.outStream->codecpar, vp.enc) < 0) {
                fail("video stream params copy failed");
                return;
            }
        }
    }

    // ---- audio output ----
    if (ap.inIndex >= 0) {
        ap.outStream = avformat_new_stream(ofmt, nullptr);
        if (ap.outStream == nullptr) {
            fail("audio output stream alloc failed");
            return;
        }
        if (acopy) {
            // Remux: codec parameters copied verbatim, timestamps rescaled at
            // write time.
            avcodec_parameters_copy(ap.outStream->codecpar, ap.inStream->codecpar);
            ap.outStream->codecpar->codec_tag = 0;
            ap.outStream->time_base = ap.inStream->time_base;
        } else {
            const AVCodecID encId = (w->acodec == "ac3") ? AV_CODEC_ID_AC3 : AV_CODEC_ID_AAC;
            const AVCodec* encCodec = avcodec_find_encoder(encId);
            if (encCodec == nullptr) {
                fail("audio encoder not found");
                return;
            }
            ap.enc = avcodec_alloc_context3(encCodec);
            ap.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
            ap.enc->sample_rate = 44100;
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            av_channel_layout_copy(&ap.enc->ch_layout, &stereo);
            ap.enc->bit_rate = w->audioBitrate;
            if (avcodec_open2(ap.enc, encCodec, nullptr) < 0) {
                fail("audio encoder open failed");
                return;
            }
            ret = swr_alloc_set_opts2(&ap.swr, &stereo, AV_SAMPLE_FMT_FLTP, ap.enc->sample_rate,
                &ap.dec->ch_layout, ap.dec->sample_fmt, ap.dec->sample_rate, 0, nullptr);
            if (ret < 0 || swr_init(ap.swr) < 0) {
                fail("swr init failed");
                return;
            }
            ap.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, ap.enc->frame_size * 2);
            if (ap.fifo == nullptr) {
                fail("audio fifo alloc failed");
                return;
            }
            ap.outStream->time_base = ap.enc->time_base;
            if (avcodec_parameters_from_context(ap.outStream->codecpar, ap.enc) < 0) {
                fail("audio stream params copy failed");
                return;
            }
        }
    }

    if (vp.outStream == nullptr && ap.outStream == nullptr) {
        fail("no encodable streams in input");
        return;
    }

    // ---- open output file + write header ----
    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt->pb, w->dst.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            fail("avio_open failed: " + ErrToStr(ret));
            return;
        }
        ofmtOpened = true;
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    ret = avformat_write_header(ofmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        fail("avformat_write_header failed: " + ErrToStr(ret));
        return;
    }

    // ---- optional seek ----
    if (w->startTimeMs > 0) {
        av_seek_frame(ifmt, -1, w->startTimeMs * 1000, AVSEEK_FLAG_BACKWARD);
    }
    int64_t endSample = -1;
    if (w->durationMs > 0 && ap.enc != nullptr) {
        endSample = av_rescale_q(w->durationMs, AVRational{ 1, 1000 },
            AVRational{ 1, ap.enc->sample_rate });
    }
    int64_t endVideoPts = AV_NOPTS_VALUE;
    if (w->durationMs > 0 && (vp.bufSink != nullptr || vcopy)) {
        // compare in input stream tb (buffersrc tb == input stream tb in the
        // encode path; copy mode compares input packets directly)
        endVideoPts = av_rescale_q(w->startTimeMs + w->durationMs, AVRational{ 1, 1000 },
            vp.inStream->time_base);
    }
    int64_t endAudioPts = AV_NOPTS_VALUE;
    if (w->durationMs > 0 && acopy) {
        endAudioPts = av_rescale_q(w->startTimeMs + w->durationMs, AVRational{ 1, 1000 },
            ap.inStream->time_base);
    }
    int64_t audioPkts = 0;
    double maxOutMs = 0.0;

    // normalize audio pts to output-sample timeline starting at 0
    auto alignAudioPts = [&ap](AVFrame* f) {
        if (f->pts == AV_NOPTS_VALUE) {
            return;
        }
        int64_t ptsSamples = av_rescale_q(f->pts, ap.inStream->time_base,
            AVRational{ 1, ap.enc->sample_rate });
        if (ap.firstPtsSamples == AV_NOPTS_VALUE) {
            ap.firstPtsSamples = ptsSamples;
        }
        int64_t norm = ptsSamples - ap.firstPtsSamples;
        if (norm > ap.nextPts) {
            ap.nextPts = norm;
        }
    };

    // ---- main loop ----
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool done = false;
    while (!done && av_read_frame(ifmt, pkt) >= 0) {
        if (pkt->stream_index == vp.inIndex) {
            if (vcopy) {
                // Remux: no decode, straight copy (optional Annex-B bsf).
                if (endVideoPts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE && pkt->pts > endVideoPts) {
                    done = true;
                } else if (!WriteCopyPacket(vp.bsf, pkt, vp.inStream->time_base, vp.outStream, ofmt,
                    w->frames, &maxOutMs)) {
                    fail("video remux write failed");
                    av_frame_free(&frame);
                    av_packet_free(&pkt);
                    return;
                }
            } else if (endVideoPts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE && pkt->pts > endVideoPts) {
                done = true;
            } else if (avcodec_send_packet(vp.dec, pkt) >= 0) {
                while (avcodec_receive_frame(vp.dec, frame) >= 0) {
                    if (endVideoPts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE &&
                        frame->pts > endVideoPts) {
                        done = true;
                        break;
                    }
                    if (!WriteVideoFrame(vp, frame, ofmt, w->frames, &maxOutMs)) {
                        av_frame_unref(frame);
                        fail("video encode failed");
                        av_frame_free(&frame);
                        av_packet_free(&pkt);
                        return;
                    }
                    av_frame_unref(frame);
                }
            }
        } else if (pkt->stream_index == ap.inIndex) {
            if (acopy) {
                // Remux: no decode, straight copy.
                if (endAudioPts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE && pkt->pts > endAudioPts) {
                    done = true;
                } else if (!WriteCopyPacket(nullptr, pkt, ap.inStream->time_base, ap.outStream, ofmt,
                    audioPkts, &maxOutMs)) {
                    fail("audio remux write failed");
                    av_frame_free(&frame);
                    av_packet_free(&pkt);
                    return;
                }
            } else if (avcodec_send_packet(ap.dec, pkt) >= 0) {
                uint8_t** convData = nullptr;
                while (avcodec_receive_frame(ap.dec, frame) >= 0) {
                    alignAudioPts(frame);
                    int outSamples = swr_get_out_samples(ap.swr, frame->nb_samples);
                    if (convData == nullptr) {
                        convData = static_cast<uint8_t**>(av_calloc(2, sizeof(uint8_t*)));
                    }
                    av_freep(&convData[0]);
                    if (av_samples_alloc(convData, nullptr, 2, outSamples, AV_SAMPLE_FMT_FLTP, 0) < 0) {
                        break;
                    }
                    int got = swr_convert(ap.swr, convData, outSamples,
                        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                    if (got > 0 && !PushAudioSamples(ap, convData, got, ofmt, audioPkts, endSample, &maxOutMs)) {
                        fail("audio encode failed");
                        break;
                    }
                    av_frame_unref(frame);
                }
                if (convData != nullptr) {
                    av_freep(&convData[0]);
                    av_free(convData);
                }
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_unref(pkt);

    // flush decoders
    if (vp.dec != nullptr && !done) {
        avcodec_send_packet(vp.dec, nullptr);
        while (avcodec_receive_frame(vp.dec, frame) >= 0) {
            if (endVideoPts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE &&
                frame->pts > endVideoPts) {
                break;
            }
            if (!WriteVideoFrame(vp, frame, ofmt, w->frames, &maxOutMs)) {
                fail("video flush encode failed");
                break;
            }
            av_frame_unref(frame);
        }
    }
    if (ap.dec != nullptr) {
        avcodec_send_packet(ap.dec, nullptr);
        uint8_t** convData = static_cast<uint8_t**>(av_calloc(2, sizeof(uint8_t*)));
        while (avcodec_receive_frame(ap.dec, frame) >= 0) {
            alignAudioPts(frame);
            int outSamples = swr_get_out_samples(ap.swr, frame->nb_samples);
            av_freep(&convData[0]);
            if (av_samples_alloc(convData, nullptr, 2, outSamples, AV_SAMPLE_FMT_FLTP, 0) < 0) {
                av_frame_unref(frame);
                break;
            }
            int got = swr_convert(ap.swr, convData, outSamples,
                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (got > 0 && !PushAudioSamples(ap, convData, got, ofmt, audioPkts, endSample, &maxOutMs)) {
                fail("audio flush encode failed");
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);
        }
        av_freep(&convData[0]);
        av_free(convData);
    }

    // flush filters/encoders, write trailer
    if (!FlushVideoPipe(vp, ofmt, w->frames, &maxOutMs)) {
        fail("video pipe flush failed");
    }
    if (!FlushAudioPipe(ap, ofmt, audioPkts, &maxOutMs)) {
        fail("audio pipe flush failed");
    }
    av_write_trailer(ofmt);

    w->outSize = (ofmt->pb != nullptr) ? avio_size(ofmt->pb) : 0;
    w->outDurMs = maxOutMs;

    av_frame_free(&frame);
    av_packet_free(&pkt);
    fail(""); // unified cleanup
}

static void ExecuteCallback(napi_env env, void* data)
{
    TranscodeWork* w = static_cast<TranscodeWork*>(data);
    RunTranscode(w);
}

static void CompleteCallback(napi_env env, napi_status status, void* data)
{
    TranscodeWork* w = static_cast<TranscodeWork*>(data);
    if (!w->err.empty()) {
        napi_value err;
        napi_value msg;
        napi_create_string_utf8(env, w->err.c_str(), NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, w->deferred, err);
    } else {
        napi_value result;
        napi_create_object(env, &result);
        napi_value v;
        napi_create_int64(env, w->outSize, &v);
        napi_set_named_property(env, result, "outputSize", v);
        napi_create_double(env, w->outDurMs, &v);
        napi_set_named_property(env, result, "durationMs", v);
        napi_create_int64(env, w->frames, &v);
        napi_set_named_property(env, result, "frames", v);
        napi_resolve_deferred(env, w->deferred, result);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}

static int64_t GetIntProp(napi_env env, napi_value obj, const char* name, int64_t dft)
{
    napi_value v = nullptr;
    bool has = false;
    if (napi_has_named_property(env, obj, name, &has) == napi_ok && has &&
        napi_get_named_property(env, obj, name, &v) == napi_ok && v != nullptr) {
        int64_t out = dft;
        if (napi_get_value_int64(env, v, &out) == napi_ok) {
            return out;
        }
    }
    return dft;
}

static void GetStringProp(napi_env env, napi_value obj, const char* name, std::string& out)
{
    napi_value v = nullptr;
    bool has = false;
    if (napi_has_named_property(env, obj, name, &has) == napi_ok && has &&
        napi_get_named_property(env, obj, name, &v) == napi_ok && v != nullptr) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) == napi_ok) {
            out.resize(len);
            if (len > 0) {
                napi_get_value_string_utf8(env, v, &out[0], len + 1, &len);
            }
        }
    }
}

static std::string GetStringArg(napi_env env, napi_value v)
{
    std::string out;
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) == napi_ok) {
        out.resize(len);
        if (len > 0) {
            napi_get_value_string_utf8(env, v, &out[0], len + 1, &len);
        }
    }
    return out;
}

} // namespace

napi_value Transcode(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2 || args[0] == nullptr || args[1] == nullptr) {
        napi_throw_error(env, nullptr, "transcode requires (src, options)");
        return nullptr;
    }

    TranscodeWork* w = new TranscodeWork();
    w->src = GetStringArg(env, args[0]);
    w->dst = "out.mp4";
    GetStringProp(env, args[1], "dst", w->dst);
    w->width = static_cast<int>(GetIntProp(env, args[1], "width", 0));
    w->height = static_cast<int>(GetIntProp(env, args[1], "height", 0));
    w->fps = static_cast<int>(GetIntProp(env, args[1], "fps", 0));
    w->videoBitrate = GetIntProp(env, args[1], "videoBitrate", 800000);
    w->audioBitrate = GetIntProp(env, args[1], "audioBitrate", 96000);
    w->startTimeMs = GetIntProp(env, args[1], "startTimeMs", 0);
    w->durationMs = GetIntProp(env, args[1], "durationMs", 0);
    GetStringProp(env, args[1], "vcodec", w->vcodec);
    GetStringProp(env, args[1], "acodec", w->acodec);
    GetStringProp(env, args[1], "filter", w->filter);
    if (!w->vcodec.empty() && w->vcodec != "h264" && w->vcodec != "mpeg2" && w->vcodec != "copy") {
        delete w;
        napi_throw_type_error(env, nullptr,
            "vcodec must be 'h264' (default, libopenh264) | 'mpeg2' (mpeg2video) | 'copy' (remux without re-encode)");
        return nullptr;
    }
    if (!w->acodec.empty() && w->acodec != "aac" && w->acodec != "ac3" && w->acodec != "copy") {
        delete w;
        napi_throw_type_error(env, nullptr,
            "acodec must be 'aac' (default) | 'ac3' | 'copy' (remux without re-encode)");
        return nullptr;
    }

    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_transcode", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteCallback, CompleteCallback,
        w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

} // namespace ffmpeg
