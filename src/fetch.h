// fetch.h — reading the feed, off the UI thread.
//
// One request at a time, cancelled by ticket rather than by handle: every
// completion checks its token against the current generation and a stale reply
// is dropped. Simpler than cancelling a WinHTTP request mid-flight, and it is
// the only failure mode that matters (the user changed calendars while a slow
// request was in the air).

#pragma once

#include <functional>
#include <string>

#include "common.h"
#include "raii.h"

namespace rc {

enum class FetchStatus {
    Ok,
    NoCalendar,        // "No calendar yet - click to set one up"
    FileUnreadable,    // "Can't read <filename>"
    FileNotCalendar,   // "That file isn't iCalendar"
    NetworkError,      // "Calendar unreachable (<description>)"
    HttpError,         // "Calendar HTTP <code> - is the feed public?"
    BadBody            // "Feed is not valid iCalendar"
};

struct FetchResult {
    FetchStatus status = FetchStatus::Ok;
    std::wstring body;      // valid only when status == Ok
    std::wstring message;   // ready to print on the strip, in every other case
    unsigned long token = 0;
    int httpCode = 0;
};

// Starts a fetch on a worker thread. When it finishes, `WM_APP_FETCH_DONE` is
// posted to `notify` with a heap-allocated FetchResult* as lParam; the window
// takes ownership and deletes it.
//
// HTTP requests set no-cache flags and a 20 second timeout, because a feed that
// takes longer than that is not going to help a widget that redraws every
// second. file:// URLs are read on the same worker thread.
//
// Returns the generation token of this request.
unsigned long StartFetch(const std::wstring& url, HWND notify);

// The token of the most recently started fetch.
unsigned long CurrentFetchToken();

// True while a request is in flight (drives the "Refreshing..." caption).
bool IsFetching();

}  // namespace rc
