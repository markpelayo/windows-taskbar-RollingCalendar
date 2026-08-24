// calsource.cpp — whatever was pasted, turned into something fetchable.
//
// The rules are ordered, and the order is the whole design. A link that already
// ends in .ics is taken at its word before anything else is tried, because that
// is the one form the user can be certain about; the Google-specific derivation
// only happens once the cheaper interpretations have been ruled out.
//
// No network here, and no URL library either: the parsing this needs amounts to
// finding a '?', a '&' and an '=', and a percent table.

#include "calsource.h"

#include <windows.h>

namespace rc {
namespace calsource {
namespace {

// RFC 3986 unreserved. Everything else is escaped, which notably includes '@',
// so a calendar address becomes you%40gmail.com in the path segment.
bool IsUnreserved(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
           c == L'-' || c == L'.' || c == L'_' || c == L'~';
}

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::wstring PercentEncode(const std::wstring& s) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    // Encoding is byte-wise, so anything non-ASCII goes through UTF-8 first.
    const std::string utf8 = Narrow(s);
    std::wstring out;
    out.reserve(utf8.size());
    for (unsigned char b : utf8) {
        if (b < 0x80 && IsUnreserved(static_cast<wchar_t>(b))) {
            out.push_back(static_cast<wchar_t>(b));
        } else {
            out.push_back(L'%');
            out.push_back(kHex[(b >> 4) & 0x0F]);
            out.push_back(kHex[b & 0x0F]);
        }
    }
    return out;
}

std::wstring PercentDecode(const std::wstring& s) {
    std::wstring out;
    std::string bytes;  // a run of %XX triplets, decoded together as UTF-8
    out.reserve(s.size());

    auto flush = [&]() {
        if (bytes.empty()) return;
        out.append(Widen(bytes));
        bytes.clear();
    };

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int hi = HexValue(s[i + 1]);
            const int lo = HexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        // A stray '%' is passed through rather than treated as an error: people
        // paste half-encoded links, and the rest of the link is still useful.
        flush();
        out.push_back(s[i]);
    }
    flush();
    return out;
}

// Everything after the first '?', split on '&', each pair split on its *first*
// '=' so that an encoded '=' inside a value survives. First match wins.
std::optional<std::wstring> QueryValue(const std::wstring& url, const std::wstring& key) {
    const size_t q = url.find(L'?');
    if (q == std::wstring::npos) return std::nullopt;

    const std::wstring wanted = Lower(key);
    for (const std::wstring& pair : Split(url.substr(q + 1), L'&')) {
        const size_t eq = pair.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (Lower(pair.substr(0, eq)) != wanted) continue;

        std::wstring value = pair.substr(eq + 1);
        for (wchar_t& c : value) {
            if (c == L'+') c = L' ';  // form encoding, which Google does use
        }
        value = PercentDecode(value);
        // An empty value counts as absent: "?src=&ctz=" should fall through to
        // the next rule rather than derive a feed URL for nobody.
        if (value.empty()) return std::nullopt;
        return value;
    }
    return std::nullopt;
}

// No scheme, no slash, but an '@': that is somebody's calendar address.
bool LooksLikeBareAddress(const std::wstring& s) {
    return s.find(L"://") == std::wstring::npos && s.find(L'/') == std::wstring::npos &&
           s.find(L'@') != std::wstring::npos;
}

std::wstring DerivedFeedUrl(const std::wstring& id) {
    return L"https://calendar.google.com/calendar/ical/" + PercentEncode(id) +
           L"/public/basic.ics";
}

// webcal:// is a Netscape-era alias for "subscribe to this over HTTPS". No
// server ever speaks it.
std::wstring ReplaceWebcal(const std::wstring& s) {
    if (StartsWith(Lower(s), L"webcal://")) {
        return L"https://" + s.substr(9);
    }
    return s;
}

const wchar_t* kSchemeless =
    L"That doesn't look like a calendar link. Paste a Google Calendar embed link, a public .ics "
    L"URL, or a calendar address like you@gmail.com.";
const wchar_t* kBadUrl = L"Couldn't turn that into a valid URL.";

}  // namespace

std::optional<std::wstring> ToIcs(const std::wstring& input) {
    const std::wstring trimmed = Trim(input);
    if (trimmed.empty()) return std::nullopt;

    const std::wstring s = ReplaceWebcal(trimmed);

    // A .ics suffix is the only way a file:// path gets through, and it is
    // deliberate: without it there is no way to tell a calendar apart from any
    // other local file the user happened to drag in.
    if (EndsWithNoCase(s, L".ics")) return s;

    // One rule for both /calendar/embed?src= and /calendar/u/0/newembed?src=.
    if (const std::optional<std::wstring> src = QueryValue(s, L"src")) {
        return DerivedFeedUrl(*src);
    }

    if (LooksLikeBareAddress(s)) return DerivedFeedUrl(s);

    if (StartsWith(Lower(s), L"http")) return s;

    return std::nullopt;
}

std::optional<std::wstring> TimeZoneOf(const std::wstring& input) {
    // Read from the original input: the derived feed URL has no ctz, and the
    // zone the calendar is published in is a property of the link the user was
    // given, not of the file it points at.
    return QueryValue(Trim(input), L"ctz");
}

std::wstring Label(const std::wstring& input) {
    const std::wstring s = Trim(input);
    if (s.empty()) return s;

    if (const std::optional<std::wstring> src = QueryValue(s, L"src")) return *src;
    if (LooksLikeBareAddress(s)) return s;

    const std::wstring marker = L"/calendar/ical/";
    const size_t at = s.find(marker);
    if (at != std::wstring::npos) {
        const size_t begin = at + marker.size();
        const size_t end = s.find(L'/', begin);
        const std::wstring segment =
            (end == std::wstring::npos) ? s.substr(begin) : s.substr(begin, end - begin);
        if (!segment.empty()) return PercentDecode(segment);
    }

    return s;
}

std::wstring Problem(const std::wstring& input) {
    const std::optional<std::wstring> url = ToIcs(input);
    if (!url) return kSchemeless;

    const std::wstring& s = *url;
    const size_t sep = s.find(L"://");
    if (sep == std::wstring::npos || sep == 0) return kBadUrl;

    const std::wstring scheme = Lower(s.substr(0, sep));

    if (scheme == L"file") {
        const std::wstring path = FileUrlToPath(s);
        if (path.empty()) return kBadUrl;
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return L"No file at " + path;
        }
        return std::wstring();
    }

    // A host is what makes the rest fetchable. http and https must have one;
    // anything else with no host is not a link we can do anything with either.
    const size_t hostBegin = sep + 3;
    size_t hostEnd = s.find_first_of(L"/?#", hostBegin);
    if (hostEnd == std::wstring::npos) hostEnd = s.size();
    if (hostEnd <= hostBegin) return kBadUrl;

    if (scheme != L"http" && scheme != L"https") return kBadUrl;

    return std::wstring();
}

bool IsFileUrl(const std::wstring& url) {
    return StartsWith(Lower(Trim(url)), L"file://");
}

std::wstring FileUrlToPath(const std::wstring& url) {
    const std::wstring s = Trim(url);
    if (!IsFileUrl(s)) return std::wstring();

    std::wstring rest = s.substr(7);  // past "file://"

    // Both file:///C:/x and file://C:/x turn up in the wild; the first is the
    // correct spelling (empty authority) and the second is what people type.
    if (rest.size() >= 3 && rest[0] == L'/' && ((rest[1] >= L'A' && rest[1] <= L'Z') ||
                                                (rest[1] >= L'a' && rest[1] <= L'z')) &&
        rest[2] == L':') {
        rest.erase(0, 1);
    }

    rest = PercentDecode(rest);
    for (wchar_t& c : rest) {
        if (c == L'/') c = L'\\';
    }
    return rest;
}

}  // namespace calsource
}  // namespace rc
