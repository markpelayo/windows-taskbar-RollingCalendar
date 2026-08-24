// ics.h — the event model and a hand-rolled RFC 5545 parser.
//
// Hand-rolled deliberately. A feed with hundreds of events, re-read every five
// minutes, would otherwise build a date-formatting object per DTSTART, DTEND,
// EXDATE and UNTIL. Parsing digits directly is both faster and, because every
// field range is checked and every parsed date is read back and verified, more
// predictable than asking a formatter to be lenient.
//
// Nothing from outside is trusted: durations are bounded, out-of-range dates
// refused, every parse is total.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.h"

namespace rc {

struct CalEvent {
    std::wstring title;
    Seconds start = 0;
    Seconds end = 0;
    bool isAllDay = false;
    std::optional<COLORREF> color;    // nil until a keyword rule matches
    std::wstring category;            // from the matching keyword rule

    double duration() const { return static_cast<double>(end - start); }
    bool intersects(Seconds from, Seconds to) const { return end > from && start < to; }
    bool runningAt(Seconds t) const { return start <= t && t < end; }
};

namespace ics {

// True when the body looks like iCalendar at all (contains BEGIN:VCALENDAR).
bool LooksLikeCalendar(const std::wstring& body);

// Parses `body` and returns every event that falls on one of `dayOffsets`
// relative to the day containing `now`, in `zone`.
//
// The offsets are the four days [-1, 0, +1, +2]: yesterday through the day
// after tomorrow, so the dropdown's sleep-to-sleep cycle can always find both
// of its boundaries.
//
// Non-recurring events are filtered to those days *as they are parsed*, so a
// calendar with years of history costs no more to refresh than one with a week
// in it. Recurring events are expanded per requested day rather than iterated
// forward from DTSTART.
//
// Supported recurrence: FREQ=DAILY|WEEKLY|MONTHLY|YEARLY with INTERVAL, BYDAY,
// BYMONTHDAY, UNTIL and COUNT, plus EXDATE and RECURRENCE-ID overrides.
// Everything else yields no occurrences rather than a wrong one.
//
// Results are de-duplicated on "title|start|end" and sorted by start ascending.
std::vector<CalEvent> Parse(const std::wstring& body,
                            Seconds now,
                            const TimeZone& zone,
                            const std::vector<int>& dayOffsets);

// The four days the app loads. Exposed so callers agree on the window.
const std::vector<int>& DefaultDayOffsets();

}  // namespace ics
}  // namespace rc
