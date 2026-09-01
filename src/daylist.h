// daylist.h — the day, listed.
//
// Clicking the strip drops down the day's blocks. Not midnight to midnight:
// anchor to anchor, so a night shift reads as one stretch rather than being
// guillotined at 00:00. The anchor is a keyword, "sleep" by default.
//
// Columns are aligned with computed tab stops, not by padding with spaces.
// Padding cannot align proportional text -- "7h" and "30m" are two and three
// characters, and 'm' is wider than 'h', so the separator drifts by a few
// pixels a row and the whole column looks drunk.

#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "ics.h"

namespace rc {

struct DayRow {
    bool isSeparator = false;
    std::wstring separatorText;    // "Thursday, August 20"

    std::wstring time;             // "04:30 AM - 11:30 AM", or "All day"
    std::wstring duration;         // "7h"; empty for all-day
    std::wstring title;
    std::wstring category;
    COLORREF categoryColor = RGB(0x8E, 0x8E, 0x93);
    int overlapCount = 0;          // badge drawn when > 1
    bool isCurrent = false;        // gets the marker and a bold title
    bool isPast = false;
    bool isMore = false;           // the "... and N more" tail row
};

namespace daylist {

// The anchor-to-anchor cycle.
//
//   1. Anchors are events whose title whole-word contains the anchor keyword.
//   2. Consecutive anchors merge into runs when the next starts within thirty
//      minutes of the previous one ending, so a sleep split into fifteen-minute
//      chunks by a tracker still counts as one boundary.
//   3. The opener is the last run starting at or before now; the closer is the
//      next run after it.
//   4. No anchors, or the day's anchor has not happened yet, falls back to
//      today plus a rolling twenty-four hours -- a day-only fallback would hide
//      everything past midnight while the strip was already showing it.
std::vector<CalEvent> CycleEvents(const std::vector<CalEvent>& all,
                                  Seconds now,
                                  const TimeZone& zone,
                                  const std::wstring& anchorKeyword);

// Formats the cycle into rows, inserting a date separator whenever the day
// changes (except before the first row when that day is today), and capping at
// sixty rows with a "... and N more" tail.
std::vector<DayRow> BuildRows(const std::vector<CalEvent>& cycle,
                              Seconds now,
                              const TimeZone& zone);

// "Week 35  .  Monday  .  August 24, 2026  .  Demo Calendar (test data)"
//
// The neutral part only. The simulated-clock marker is deliberately not
// appended: it has to be drawn red and bold against a caption that is dim and
// normal weight, which one string cannot express.
std::wstring Caption(Seconds now, const TimeZone& zone, const std::wstring& sourceName);

// "  .  ! Simulated !" while Debug Time is in force, otherwise empty. The
// caller draws it as a second run after Caption's text.
std::wstring SimulatedSuffix();

// "Updated just now" / "Updated 3 minutes ago" / "Updated at 4:05:11 PM" /
// "Refreshing..." / "Not read yet". Age uses the real clock, not the simulated
// one -- how stale the data is has nothing to do with Debug Time.
std::wstring FreshnessCaption(Seconds lastFetch, bool fetching);

constexpr int kMaxRows = 60;
constexpr Seconds kAnchorJoinTolerance = 1800;

}  // namespace daylist
}  // namespace rc
