// Host-side unit tests for the pure HLS parsing layer (hls_parse.cpp).
// No FFmpeg/OHOS dependency — builds and runs anywhere with a C++17
// compiler. Build & run via tools/host-tests.sh.
#include "hls_parse.h"

#include <cstdio>
#include <string>
#include <vector>

using ffmpeg::HlsPlaylistDoc;
using ffmpeg::HlsVariant;
using ffmpeg::ParseHlsPlaylist;
using ffmpeg::ParseTargetDurationSec;
using ffmpeg::ResolveUrl;
using ffmpeg::SelectVariant;

namespace {

int g_failures = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

void CheckEq(const std::string& got, const std::string& want, const char* what)
{
    if (got == want) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s (got \"%s\", want \"%s\")\n", what, got.c_str(), want.c_str());
        ++g_failures;
    }
}

const char* kMaster = "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=640x360,CODECS=\"avc1.42e00a,mp4a.40.2\"\n"
    "lo/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=4128000,RESOLUTION=1920x1080,CODECS=\"avc1.640028,mp4a.40.2\"\n"
    "hi/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2560000,RESOLUTION=1280x720,CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
    "https://cdn.other.example/abs/med.m3u8\n";

void TestResolveUrl()
{
    std::printf("TestResolveUrl\n");
    const std::string base = "https://h.example.com/a/b/playlist.m3u8";
    CheckEq(ResolveUrl(base, "seg.ts"), "https://h.example.com/a/b/seg.ts", "relative path");
    CheckEq(ResolveUrl(base, "/root/seg.ts"), "https://h.example.com/root/seg.ts", "absolute path");
    CheckEq(ResolveUrl(base, "//cdn.example.com/x.m3u8"), "https://cdn.example.com/x.m3u8",
        "protocol-relative keeps scheme");
    CheckEq(ResolveUrl(base, "http://plain.example.com/y.m3u8"), "http://plain.example.com/y.m3u8",
        "absolute passes through");
    CheckEq(ResolveUrl(base, ""), base, "empty ref returns base");
    CheckEq(ResolveUrl("https://h.example.com", "z.m3u8"), "https://h.example.com/z.m3u8",
        "base without path");
    CheckEq(ResolveUrl("file/local.m3u8", "seg.ts"), "seg.ts", "non-http base passes ref through");
    CheckEq(ResolveUrl(base, "sub/dir/seg.ts"), "https://h.example.com/a/b/sub/dir/seg.ts",
        "nested relative path");
}

void TestParseMaster()
{
    std::printf("TestParseMaster\n");
    HlsPlaylistDoc doc;
    ParseHlsPlaylist(kMaster, "https://h.example.com/a/b/master.m3u8", doc);
    Check(doc.isMaster, "master detected");
    Check(doc.variants.size() == 3, "three variants parsed");
    if (doc.variants.size() == 3) {
        CheckEq(doc.variants[0].uri, "https://h.example.com/a/b/lo/index.m3u8", "v0 uri resolved");
        Check(doc.variants[0].bandwidth == 1280000, "v0 bandwidth");
        Check(doc.variants[0].width == 640 && doc.variants[0].height == 360, "v0 resolution");
        CheckEq(doc.variants[0].codecs, "avc1.42e00a,mp4a.40.2", "v0 quoted codecs");
        Check(doc.variants[2].bandwidth == 2560000, "v2 bandwidth");
        CheckEq(doc.variants[2].uri, "https://cdn.other.example/abs/med.m3u8",
            "v2 absolute uri untouched");
    }

    // Media playlist: no STREAM-INF.
    HlsPlaylistDoc media;
    ParseHlsPlaylist("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:5.9,\nseg0.ts\n",
        "https://h.example.com/live.m3u8", media);
    Check(!media.isMaster && media.variants.empty(), "media playlist not master");

    // Not an M3U8 at all (HTML error page).
    HlsPlaylistDoc junk;
    ParseHlsPlaylist("<html><body>404</body></html>", "https://h.example.com/x.m3u8", junk);
    Check(!junk.isMaster && junk.variants.empty(), "non-m3u8 rejected");

    // CRLF line endings (Windows/CDN normalization) and a comment line
    // between the tag and the URI: the pairing is invalidated per spec, but
    // later pairs still parse.
    const std::string crlf = "#EXTM3U\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000\r\n"
        "# comment interrupts the pairing\r\n"
        "orphan.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000\r\n"
        "ok.m3u8\r\n";
    HlsPlaylistDoc doc2;
    ParseHlsPlaylist(crlf, "https://h.example.com/m.m3u8", doc2);
    Check(doc2.isMaster, "crlf master detected");
    Check(doc2.variants.size() == 1, "comment broke the pairing, one variant left");
    if (doc2.variants.size() == 1) {
        Check(doc2.variants[0].bandwidth == 2000000, "crlf variant bandwidth");
        CheckEq(doc2.variants[0].uri, "https://h.example.com/ok.m3u8", "crlf variant uri");
    }
}

void TestTargetDuration()
{
    std::printf("TestTargetDuration\n");
    Check(ParseTargetDurationSec("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:9.5,\ns.ts\n") == 6,
        "explicit tag wins over larger extinf");
    Check(ParseTargetDurationSec("#EXTM3U\n#EXTINF:2.0,\na.ts\n#EXTINF:5.9,\nb.ts\n") == 6,
        "fallback ceil of max extinf");
    Check(ParseTargetDurationSec("#EXTM3U\n#EXTINF:6.0,\na.ts\n") == 6,
        "fallback exact integer");
    Check(ParseTargetDurationSec("#EXTM3U\n") == -1, "absent -> -1");
    Check(ParseTargetDurationSec("") == -1, "empty -> -1");
    Check(ParseTargetDurationSec("#EXTM3U\n#EXT-X-TARGETDURATION:10\r\n#EXTINF:3.2,\na.ts\r\n") == 10,
        "crlf tolerated");
}

std::vector<HlsVariant> MakeVariants(const int64_t* bandwidths, size_t n)
{
    std::vector<HlsVariant> v;
    for (size_t i = 0; i < n; ++i) {
        HlsVariant variant;
        variant.bandwidth = bandwidths[i];
        v.push_back(variant);
    }
    return v;
}

void TestSelectVariant()
{
    std::printf("TestSelectVariant\n");
    const int64_t bws[] = { 1280000, 4128000, 2560000 };
    const auto variants = MakeVariants(bws, 3);

    Check(SelectVariant(variants, 1, 0) == 1, "explicit index selects directly");
    Check(SelectVariant(variants, 5, 0) == -1, "out-of-range index ignored");
    Check(SelectVariant(variants, -1, 0) == -1, "no index and no cap -> -1");

    Check(SelectVariant(variants, INT32_MIN, 2000000) == 0, "cap picks best <= cap");
    Check(SelectVariant(variants, INT32_MIN, 3000000) == 2, "cap picks higher under-cap variant");
    Check(SelectVariant(variants, INT32_MIN, 5000000) == 1, "cap above all picks the top");
    Check(SelectVariant(variants, INT32_MIN, 1000000) == 0,
        "all exceed cap -> lowest bandwidth");

    const int64_t tie[] = { 1000000, 1000000 };
    Check(SelectVariant(MakeVariants(tie, 2), INT32_MIN, 1500000) == 0, "tie keeps the first");

    const int64_t unknown[] = { 0, 0 };
    Check(SelectVariant(MakeVariants(unknown, 2), INT32_MIN, 1500000) == -1,
        "no declared bandwidths -> -1");

    Check(SelectVariant(std::vector<HlsVariant>(), 0, 0) == -1, "empty list -> -1");
}

} // namespace

int main()
{
    TestResolveUrl();
    TestParseMaster();
    TestTargetDuration();
    TestSelectVariant();
    if (g_failures == 0) {
        std::printf("test_hls_parse: ALL PASS\n");
        return 0;
    }
    std::printf("test_hls_parse: %d FAILURE(S)\n", g_failures);
    return 1;
}
