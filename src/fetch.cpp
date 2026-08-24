// fetch.cpp — the feed reader.
//
// See fetch.h for the contract. The design worth restating here is the ticket:
// every request carries a generation number, and the window that receives the
// result compares it against the current generation and drops anything stale.
// Nothing is ever cancelled mid-flight. Cancelling a WinHTTP request properly
// means closing handles from a second thread while the first is blocked inside
// WinHttpReceiveResponse, which is a race with a shutdown path attached; a
// number comparison on the UI thread is neither, and the only case that matters
// -- the user switched calendars while a slow request was in the air -- is
// handled identically by both.
//
// User-visible strings that contain an em dash are written with \u2014 rather
// than the character itself, so the file's encoding cannot change what the user
// reads.

#include "fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <string>

#include "app.h"
#include "ics.h"

#pragma comment(lib, "winhttp.lib")

namespace rc {
namespace {

// The generation counter and the in-flight count. Both are touched from worker
// threads, so both go through the Interlocked family rather than volatile alone.
volatile LONG g_generation = 0;
volatile LONG g_inFlight = 0;

// A feed that will not fit in this is not a day's calendar, it is a mistake or
// an attack; either way there is no reason to grow a buffer for it.
const size_t kMaxBodyBytes = 32u * 1024u * 1024u;

const DWORD kTimeoutMs = 20000;  // section 2.2: 20 s, all four phases

struct InternetDeleter {
    void operator()(HINTERNET h) const { ::WinHttpCloseHandle(h); }
};
using UniqueInternet = Unique<HINTERNET, InternetDeleter>;

struct FetchJob {
    std::wstring url;
    HWND notify = nullptr;
    unsigned long token = 0;
};

std::wstring PercentDecode(const std::wstring& s) {
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<wchar_t>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// "file:///C:/x/y.ics" -> "C:\x\y.ics". A bare path is returned unchanged, so
// callers that never built a URL still work.
std::wstring FileUrlToPath(const std::wstring& url) {
    std::wstring rest = url;
    if (StartsWith(Lower(rest), L"file://")) {
        rest = rest.substr(7);
        if (StartsWith(Lower(rest), L"localhost/")) rest = rest.substr(9);
        // A local path arrives as an empty authority plus an absolute path, so
        // "file:///C:/..." carries a leading slash that is not part of it.
        if (rest.size() >= 3 && rest[0] == L'/' && rest[2] == L':') rest = rest.substr(1);
    }
    rest = PercentDecode(rest);
    for (wchar_t& c : rest) {
        if (c == L'/') c = L'\\';
    }
    return rest;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

std::wstring DescribeWinHttpError(DWORD err) {
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT:
            return L"timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return L"server not found";
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return L"cannot connect";
        case ERROR_WINHTTP_SECURE_FAILURE:
            return L"secure connection failed";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return L"connection lost";
        default:
            return Format(L"error %lu", err);
    }
}

void Deliver(HWND notify, FetchResult* result) {
    if (!result) return;
    if (!notify || !::PostMessageW(notify, WM_APP_FETCH_DONE, 0,
                                   reinterpret_cast<LPARAM>(result))) {
        // Nobody will ever take ownership, so it is ours to release.
        delete result;
    }
}

void FetchFile(const std::wstring& url, FetchResult* result) {
    const std::wstring path = FileUrlToPath(url);
    std::wstring text;
    if (!ReadFileText(path, &text)) {
        result->status = FetchStatus::FileUnreadable;
        result->message = Format(L"Can't read %s", FileNameOf(path).c_str());
        return;
    }
    if (!ics::LooksLikeCalendar(text)) {
        result->status = FetchStatus::FileNotCalendar;
        result->message = L"That file isn't iCalendar";
        return;
    }
    result->status = FetchStatus::Ok;
    result->body = std::move(text);
}

void FetchHttp(const std::wstring& url, FetchResult* result) {
    auto networkFailure = [&](DWORD err) {
        result->status = FetchStatus::NetworkError;
        result->message =
            Format(L"Calendar unreachable (%s)", DescribeWinHttpError(err).c_str());
    };

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    wchar_t extra[2048] = {0};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);

    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        networkFailure(::GetLastError());
        return;
    }

    UniqueInternet session(::WinHttpOpen(L"RollingCalendar/1.0",
                                         WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        // Automatic proxy discovery needs Windows 8.1 or later; older machines
        // get the statically configured proxy instead of no calendar at all.
        session.reset(::WinHttpOpen(L"RollingCalendar/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session) {
        networkFailure(::GetLastError());
        return;
    }
    ::WinHttpSetTimeouts(session.get(), static_cast<int>(kTimeoutMs), static_cast<int>(kTimeoutMs),
                         static_cast<int>(kTimeoutMs), static_cast<int>(kTimeoutMs));

    UniqueInternet connect(::WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connect) {
        networkFailure(::GetLastError());
        return;
    }

    std::wstring target(path);
    target += extra;

    // WINHTTP_FLAG_REFRESH is the point of this whole request: a calendar read
    // from the WinINet cache is exactly the stale day the refresh exists to
    // replace. Redirects are followed by the default policy, because publishing
    // an .ics almost always produces one.
    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;

    UniqueInternet request(::WinHttpOpenRequest(connect.get(), L"GET", target.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                flags));
    if (!request) {
        networkFailure(::GetLastError());
        return;
    }

    if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request.get(), nullptr)) {
        networkFailure(::GetLastError());
        return;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!::WinHttpQueryHeaders(request.get(),
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                               WINHTTP_NO_HEADER_INDEX)) {
        networkFailure(::GetLastError());
        return;
    }
    result->httpCode = static_cast<int>(statusCode);
    if (statusCode != 200) {
        result->status = FetchStatus::HttpError;
        result->message =
            Format(L"Calendar HTTP %d \u2014 is the feed public?", static_cast<int>(statusCode));
        return;
    }

    std::string bytes;
    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.get(), &available)) {
            networkFailure(::GetLastError());
            return;
        }
        if (available == 0) break;
        if (bytes.size() + available > kMaxBodyBytes) {
            result->status = FetchStatus::BadBody;
            result->message = L"Feed is not valid iCalendar";
            return;
        }
        const size_t offset = bytes.size();
        bytes.resize(offset + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request.get(), &bytes[offset], available, &read)) {
            networkFailure(::GetLastError());
            return;
        }
        bytes.resize(offset + read);
        if (read == 0) break;
    }

    std::wstring text = DecodeText(bytes);
    if (!ics::LooksLikeCalendar(text)) {
        result->status = FetchStatus::BadBody;
        result->message = L"Feed is not valid iCalendar";
        return;
    }
    result->status = FetchStatus::Ok;
    result->body = std::move(text);
}

DWORD WINAPI FetchThread(LPVOID param) {
    FetchJob* job = static_cast<FetchJob*>(param);
    FetchResult* result = nullptr;

    // Nothing may escape this thread: an exception crossing the thread
    // procedure takes the process with it, and the input here is a URL from a
    // stranger's web server.
    try {
        result = new FetchResult();
        result->token = job->token;
        if (StartsWith(Lower(job->url), L"http")) {
            FetchHttp(job->url, result);
        } else {
            FetchFile(job->url, result);
        }
    } catch (...) {
        if (!result) {
            ::InterlockedDecrement(&g_inFlight);
            delete job;
            return 0;
        }
        result->status = FetchStatus::NetworkError;
        result->body.clear();
        result->message = L"Calendar unreachable (internal error)";
    }

    const HWND notify = job->notify;
    delete job;
    ::InterlockedDecrement(&g_inFlight);
    Deliver(notify, result);
    return 0;
}

}  // namespace

unsigned long StartFetch(const std::wstring& url, HWND notify) {
    const unsigned long token = static_cast<unsigned long>(::InterlockedIncrement(&g_generation));

    if (Trim(url).empty()) {
        FetchResult* result = new FetchResult();
        result->token = token;
        result->status = FetchStatus::NoCalendar;
        result->message = L"No calendar yet \u2014 click to set one up";
        Deliver(notify, result);
        return token;
    }

    FetchJob* job = new FetchJob();
    job->url = Trim(url);
    job->notify = notify;
    job->token = token;

    ::InterlockedIncrement(&g_inFlight);

    // A plain CreateThread, not a pool: this is one short-lived request every
    // five minutes, and a thread pool would add a dependency and a shutdown
    // ordering problem to save an allocation nobody will ever measure.
    HANDLE thread = ::CreateThread(nullptr, 0, FetchThread, job, 0, nullptr);
    if (!thread) {
        ::InterlockedDecrement(&g_inFlight);
        const DWORD err = ::GetLastError();
        delete job;
        FetchResult* result = new FetchResult();
        result->token = token;
        result->status = FetchStatus::NetworkError;
        result->message = Format(L"Calendar unreachable (%s)", DescribeWinHttpError(err).c_str());
        Deliver(notify, result);
        return token;
    }
    // The result comes back by message, so the handle is of no further use.
    ::CloseHandle(thread);
    return token;
}

unsigned long CurrentFetchToken() {
    return static_cast<unsigned long>(::InterlockedCompareExchange(&g_generation, 0, 0));
}

bool IsFetching() { return ::InterlockedCompareExchange(&g_inFlight, 0, 0) > 0; }

}  // namespace rc
