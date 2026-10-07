#include "hls_playlist.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <hilog/log.h>

#include <cmath>
#include <vector>

namespace ffmpeg {
namespace {

// Playlist fetch cap: a master playlist is a few KB; media playlists of DVR
// windows with thousands of segments can reach hundreds of KB. 1 MiB is far
// beyond either while keeping a hostile server from OOM-ing the process.
constexpr size_t MAX_PLAYLIST_BYTES = 1024 * 1024;

// Fetch a URL as text over FFmpeg avio. Returns false on open/read failure or
// interrupt; `out` holds at most MAX_PLAYLIST_BYTES.
bool FetchText(const std::string& url, const NetOptions& opts,
    int (*interruptCb)(void*), void* interruptOpaque, std::string& out)
{
    out.clear();
    AVIOContext* avio = nullptr;
    AVDictionary* dict = nullptr;
    if (!opts.userAgent.empty()) {
        av_dict_set(&dict, "user_agent", opts.userAgent.c_str(), 0);
    }
    if (!opts.headers.empty()) {
        av_dict_set(&dict, "headers", opts.headers.c_str(), 0);
    }
    AVIOInterruptCB cb;
    cb.callback = interruptCb;
    cb.opaque = interruptOpaque;
    const int openRet = avio_open2(&avio, url.c_str(), AVIO_FLAG_READ, &cb, &dict);
    av_dict_free(&dict);
    if (openRet < 0 || avio == nullptr) {
        return false;
    }
    std::vector<char> buf;
    buf.reserve(4096);
    bool failed = false;
    while (buf.size() < MAX_PLAYLIST_BYTES) {
        // Read in chunks; avio_read blocks per the protocol's own buffering.
        unsigned char chunk[4096];
        const int n = avio_read(avio, chunk, static_cast<int>(sizeof(chunk)));
        if (n < 0) {
            failed = n != AVERROR_EOF; // clean EOF ends the body
            break;
        }
        if (n == 0) {
            break;
        }
        buf.insert(buf.end(), chunk, chunk + n);
    }
    avio_closep(&avio);
    if (failed) {
        return false;
    }
    if (buf.empty()) {
        out.clear();
    } else {
        out.assign(buf.data(), buf.size());
    }
    return true;
}

// F1: derive live_start_index from a target latency and the media playlist's
// segment duration. live_start_index counts back from the newest segment
// (-1 = live edge), so the needed backlog is ceil(target/segDur) segments.
int32_t LatencyToLiveStartIndex(int64_t targetLatencyMs, const std::string& mediaPlaylistText)
{
    const int32_t targetSec = ParseTargetDurationSec(mediaPlaylistText);
    if (targetSec <= 0 || targetLatencyMs <= 0) {
        return INT32_MIN;
    }
    const int64_t targetMs = targetLatencyMs;
    const int64_t segMs = static_cast<int64_t>(targetSec) * 1000;
    const int64_t count = (targetMs + segMs - 1) / segMs; // ceil
    const int32_t idx = -static_cast<int32_t>(count < 1 ? 1 : (count > 30 ? 30 : count));
    return idx;
}

} // namespace

SourceResolution ResolveSource(const std::string& src, const NetOptions& opts,
    int (*interruptCb)(void*), void* interruptOpaque)
{
    SourceResolution res;
    res.url = src;

    const bool wantVariant = opts.variantIndex != INT32_MIN || opts.maxBandwidth > 0;
    // F1 only when the caller did not pin an explicit live_start_index.
    const bool wantLatency =
        opts.targetLatencyMs > 0 && opts.liveStartIndex == INT32_MIN && opts.live;
    if (!wantVariant && !wantLatency) {
        return res; // nothing to resolve: keep the original URL
    }

    std::string text;
    if (!FetchText(src, opts, interruptCb, interruptOpaque, text)) {
        OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
            "playlist resolve: fetch failed for %{public}s, falling back to original url",
            src.c_str());
        return res;
    }

    HlsPlaylistDoc doc;
    ParseHlsPlaylist(text, src, doc);

    if (doc.isMaster) {
        if (wantVariant) {
            const int sel = SelectVariant(doc.variants, opts.variantIndex, opts.maxBandwidth);
            if (sel >= 0) {
                res.url = doc.variants[static_cast<size_t>(sel)].uri;
                res.variantSelected = true;
                OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFDemux",
                    "playlist resolve: variant #%{public}d bw=%{public}lld (%{public}dx%{public}d) of %{public}zu",
                    sel, static_cast<long long>(doc.variants[static_cast<size_t>(sel)].bandwidth),
                    doc.variants[static_cast<size_t>(sel)].width,
                    doc.variants[static_cast<size_t>(sel)].height, doc.variants.size());
            } else {
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                    "playlist resolve: no selectable variant (%{public}zu entries), keeping original url",
                    doc.variants.size());
            }
        }
        if (wantLatency) {
            // Segment duration lives in the media playlist: fetch the chosen
            // variant (or the original URL when no variant was selected).
            std::string mediaText;
            if (FetchText(res.url, opts, interruptCb, interruptOpaque, mediaText)) {
                res.liveStartIndex = LatencyToLiveStartIndex(opts.targetLatencyMs, mediaText);
            } else {
                OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "FFDemux",
                    "playlist resolve: media playlist fetch failed, keep default live_start_index");
            }
        }
        return res;
    }

    // The source is a media playlist (or not HLS at all): only F1 applies,
    // using the text we already fetched.
    if (wantLatency) {
        res.liveStartIndex = LatencyToLiveStartIndex(opts.targetLatencyMs, text);
    }
    return res;
}

} // namespace ffmpeg
