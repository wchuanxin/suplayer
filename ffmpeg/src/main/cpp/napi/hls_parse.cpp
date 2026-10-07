#include "hls_parse.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ffmpeg {
namespace {

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string Trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsSpace(s[b])) {
        ++b;
    }
    while (e > b && IsSpace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

// Case-insensitive prefix match.
bool HasPrefix(const std::string& s, const char* prefix)
{
    const size_t n = strlen(prefix);
    if (s.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (tolower(static_cast<unsigned char>(s[i])) !=
            tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool StartsWithHttp(const std::string& s)
{
    return HasPrefix(s, "http://") || HasPrefix(s, "https://");
}

// Find an attribute value in an EXT-X-STREAM-INF attribute list.
// Handles quoted-string (CODECS="...") and decimal (BANDWIDTH=123) forms.
std::string AttrValue(const std::string& attrs, const char* name)
{
    const size_t nameLen = strlen(name);
    size_t pos = 0;
    while (pos < attrs.size()) {
        // skip leading separators
        while (pos < attrs.size() && (attrs[pos] == ',' || IsSpace(attrs[pos]))) {
            ++pos;
        }
        const size_t eq = attrs.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }
        const std::string key = Trim(attrs.substr(pos, eq - pos));
        size_t valStart = eq + 1;
        while (valStart < attrs.size() && IsSpace(attrs[valStart])) {
            ++valStart;
        }
        std::string value;
        size_t next = 0;
        if (valStart < attrs.size() && attrs[valStart] == '"') {
            const size_t endQuote = attrs.find('"', valStart + 1);
            if (endQuote == std::string::npos) {
                break;
            }
            value = attrs.substr(valStart + 1, endQuote - valStart - 1);
            next = endQuote + 1;
        } else {
            size_t comma = attrs.find(',', valStart);
            if (comma == std::string::npos) {
                comma = attrs.size();
            }
            value = Trim(attrs.substr(valStart, comma - valStart));
            next = comma;
        }
        if (key == name) {
            return value;
        }
        pos = next;
    }
    return std::string();
}

int64_t ParseInt64(const std::string& s, int64_t fallback)
{
    if (s.empty()) {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const long long v = strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || errno == ERANGE) {
        return fallback;
    }
    return static_cast<int64_t>(v);
}

// Directory part of a URL including the trailing '/' ("http://h/a/b.m3u8" ->
// "http://h/a/"). Returns the authority root when the path has no '/'.
std::string UrlDirectory(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return std::string();
    }
    const size_t pathStart = url.find('/', scheme + 3);
    if (pathStart == std::string::npos) {
        return url + "/";
    }
    const size_t lastSlash = url.rfind('/');
    return url.substr(0, lastSlash + 1);
}

} // namespace

std::string ResolveUrl(const std::string& base, const std::string& ref)
{
    if (ref.empty()) {
        return base;
    }
    if (StartsWithHttp(ref)) {
        return ref; // already absolute
    }
    if (!StartsWithHttp(base)) {
        return ref; // no usable base (non-http source): pass through
    }
    const size_t scheme = base.find("://");
    const size_t authorityEnd = base.find('/', scheme + 3);
    const std::string root =
        (authorityEnd == std::string::npos) ? base + "/" : base.substr(0, authorityEnd);
    if (!ref.empty() && ref[0] == '/') {
        if (ref.size() > 1 && ref[1] == '/') {
            // protocol-relative: keep the base scheme
            return base.substr(0, scheme) + ":" + ref;
        }
        return root + ref; // absolute path
    }
    return UrlDirectory(base) + ref; // relative path
}

void ParseHlsPlaylist(const std::string& text, const std::string& baseUrl, HlsPlaylistDoc& out)
{
    out.isMaster = false;
    out.variants.clear();
    if (text.find("#EXTM3U") == std::string::npos) {
        return; // not an M3U8 document
    }
    size_t pos = 0;
    std::string pendingStreamInfAttrs;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        const bool haveLine = eol != std::string::npos;
        if (!haveLine) {
            eol = text.size();
        }
        std::string line = Trim(text.substr(pos, eol - pos));
        if (!pendingStreamInfAttrs.empty()) {
            if (!line.empty() && line[0] != '#') {
                HlsVariant v;
                v.uri = ResolveUrl(baseUrl, line);
                v.bandwidth = ParseInt64(AttrValue(pendingStreamInfAttrs, "BANDWIDTH"), 0);
                const std::string res = AttrValue(pendingStreamInfAttrs, "RESOLUTION");
                if (!res.empty()) {
                    const size_t x = res.find('x');
                    if (x != std::string::npos) {
                        v.width = static_cast<int32_t>(ParseInt64(res.substr(0, x), 0));
                        v.height = static_cast<int32_t>(ParseInt64(res.substr(x + 1), 0));
                    }
                }
                v.codecs = AttrValue(pendingStreamInfAttrs, "CODECS");
                out.variants.push_back(v);
                out.isMaster = true;
            }
            pendingStreamInfAttrs.clear();
            // A comment line between the tag and the URI invalidates the
            // pairing per spec; keep scanning from the next line either way.
        } else if (HasPrefix(line, "#EXT-X-STREAM-INF:")) {
            pendingStreamInfAttrs = line.substr(strlen("#EXT-X-STREAM-INF:"));
        }
        if (!haveLine) {
            break;
        }
        pos = eol + 1;
    }
}

int32_t ParseTargetDurationSec(const std::string& text)
{
    size_t pos = 0;
    double maxExtinf = -1.0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        const bool haveLine = eol != std::string::npos;
        if (!haveLine) {
            eol = text.size();
        }
        const std::string line = Trim(text.substr(pos, eol - pos));
        if (HasPrefix(line, "#EXT-X-TARGETDURATION:")) {
            return static_cast<int32_t>(
                ParseInt64(line.substr(strlen("#EXT-X-TARGETDURATION:")), -1));
        }
        if (HasPrefix(line, "#EXTINF:")) {
            const std::string val = line.substr(strlen("#EXTINF:"));
            const size_t comma = val.find(',');
            const std::string num = Trim(comma == std::string::npos ? val : val.substr(0, comma));
            if (!num.empty()) {
                const double d = strtod(num.c_str(), nullptr);
                if (d > maxExtinf) {
                    maxExtinf = d;
                }
            }
        }
        if (!haveLine) {
            break;
        }
        pos = eol + 1;
    }
    if (maxExtinf > 0.0) {
        return static_cast<int32_t>(maxExtinf + 0.999);
    }
    return -1;
}

int SelectVariant(const std::vector<HlsVariant>& variants, int32_t variantIndex, int64_t maxBandwidth)
{
    if (variants.empty()) {
        return -1;
    }
    if (variantIndex >= 0 && static_cast<size_t>(variantIndex) < variants.size()) {
        return static_cast<int>(variantIndex);
    }
    if (maxBandwidth <= 0) {
        return -1;
    }
    int best = -1;
    int bestBandwidth = -1;
    int lowest = -1;
    int lowestBandwidth = INT32_MAX;
    for (size_t i = 0; i < variants.size(); ++i) {
        const int64_t bw = variants[i].bandwidth > 0 ? variants[i].bandwidth : 0;
        if (bw > 0 && bw <= maxBandwidth && bw > bestBandwidth) {
            best = static_cast<int>(i);
            bestBandwidth = static_cast<int>(bw);
        }
        if (bw > 0 && bw < lowestBandwidth) {
            lowest = static_cast<int>(i);
            lowestBandwidth = static_cast<int>(bw);
        }
    }
    if (best >= 0) {
        return best;
    }
    // Every declared variant exceeds the cap (or none declared a bandwidth):
    // take the lowest known one rather than the original (which would bypass
    // selection entirely); fall back to -1 when nothing is known.
    return lowest;
}

} // namespace ffmpeg
