#ifndef FFMPEG_NAPI_HLS_PARSE_H
#define FFMPEG_NAPI_HLS_PARSE_H

#include <cstdint>
#include <string>
#include <vector>

namespace ffmpeg {

// One EXT-X-STREAM-INF variant of an HLS master playlist.
struct HlsVariant {
    std::string uri;       // absolute URL (resolved against the playlist URL)
    int64_t bandwidth = 0; // BANDWIDTH attribute, bits per second; 0 = unknown
    int32_t width = 0;     // RESOLUTION width; 0 = unknown
    int32_t height = 0;    // RESOLUTION height; 0 = unknown
    std::string codecs;    // CODECS attribute; empty = unknown
};

// Parsed view of an HLS playlist document. `isMaster` false = media playlist
// (or not an M3U8 at all).
struct HlsPlaylistDoc {
    bool isMaster = false;
    std::vector<HlsVariant> variants;
};

// Resolve `ref` against `base` (RFC 3986-ish, enough for HLS): absolute
// http(s) URLs pass through; protocol-relative, absolute-path and
// relative-path refs resolve against the base's scheme/authority/directory.
// Returns `ref` unchanged when the base is not an absolute http(s) URL.
std::string ResolveUrl(const std::string& base, const std::string& ref);

// Parse M3U8 text. Detects a master playlist by the presence of
// EXT-X-STREAM-INF tags and extracts the variant list (URI on the line
// following each tag, BANDWIDTH / RESOLUTION / CODECS attributes).
// A media playlist (or non-M3U8 content) yields isMaster = false.
void ParseHlsPlaylist(const std::string& text, const std::string& baseUrl, HlsPlaylistDoc& out);

// EXT-X-TARGETDURATION value in seconds; -1 when absent. Falls back to the
// ceil of the largest EXTINF duration when the tag is missing (spec requires
// the tag, but be liberal in what we accept).
int32_t ParseTargetDurationSec(const std::string& text);

// Variant selection (A2):
//  - explicit variantIndex in [0, n) selects that variant;
//  - else maxBandwidth > 0 selects the highest-bandwidth variant whose
//    bandwidth is <= maxBandwidth (ties: later wins); when every variant
//    exceeds the cap the lowest-bandwidth one is chosen;
//  - otherwise -1 (no rewrite: keep the original URL).
int SelectVariant(const std::vector<HlsVariant>& variants, int32_t variantIndex, int64_t maxBandwidth);

} // namespace ffmpeg

#endif // FFMPEG_NAPI_HLS_PARSE_H
