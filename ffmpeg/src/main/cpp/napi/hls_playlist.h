#ifndef FFMPEG_NAPI_HLS_PLAYLIST_H
#define FFMPEG_NAPI_HLS_PLAYLIST_H

#include "hls_parse.h"
#include "net_options.h"

#include <string>

namespace ffmpeg {

// Result of resolving a user-facing source URL against its playlist.
struct SourceResolution {
    // Effective URL to hand to avformat_open_input: the selected variant's
    // URL, or the original src when no variant rewrite applies / resolution
    // failed (fallback keeps the pre-1.8.0 behavior).
    std::string url;
    // True when `url` is a variant URL picked from a master playlist.
    bool variantSelected = false;
    // Derived live_start_index (F1 targetLatencyMs); INT32_MIN = keep the
    // caller's NetOptions value.
    int32_t liveStartIndex = INT32_MIN;
    // True when the fetched source text is verifiably an HLS playlist
    // (master or media). Only ever set when the resolution actually fetched
    // the playlist (wantVariant/wantLatency); a fetch failure leaves it
    // false so callers keep the stock probe-based open.
    bool isHlsPlaylist = false;
};

// FFmpeg 7.1's hls_probe rejects playlist URLs whose path lacks .m3u8/.m3u
// while the response MIME is not mpegurl (e.g. .php disguised live playlists
// served as text/plain) — the allowedExtensions/extension_picky hints only
// relax SEGMENT checks (test_segment) after the demuxer is chosen and cannot
// rescue the probe. When ResolveSource already fetched and validated the
// playlist, presetting iformat makes avformat_open_input skip probing
// entirely. No-op when fmt is null or iformat is already set.
void ForceHlsInputFormat(AVFormatContext* fmt);

// A2 variant selection + F1 latency-derived live start index.
//
// Fetches the source playlist over FFmpeg avio (so UA/headers/TLS behavior
// matches the demuxer) and:
//  - master playlist + variantIndex/maxBandwidth -> variant URL;
//  - targetLatencyMs (and no explicit liveStartIndex) -> derive
//    live_start_index = -ceil(target / segmentDuration) from the media
//    playlist's EXT-X-TARGETDURATION (falls back to the master when the
//    media playlist cannot be fetched).
//
// INFALLIBLE by design: any fetch/parse failure logs and falls back to the
// original URL (the demuxer then handles it exactly as before 1.8.0).
// `interruptCb`/`interruptOpaque` abort the fetch (same callback shape as
// AVFormatContext.interrupt_callback).
SourceResolution ResolveSource(const std::string& src, const NetOptions& opts,
    int (*interruptCb)(void*), void* interruptOpaque);

} // namespace ffmpeg

#endif // FFMPEG_NAPI_HLS_PLAYLIST_H
