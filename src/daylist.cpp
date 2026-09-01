// daylist.cpp — the day, listed. See daylist.h for the contract.
//
// Two rules govern every string built here, and both are the same rule wearing
// different hats: nothing in this file may consult the user's locale.
//
// The times and the date separators are laid out against tab stops computed
// from the widest time and duration strings in the list. GetTimeFormat and
// GetDateFormat would honour the user's regional settings, so on one machine
// the time column would read "4:30 AM" and on another "04:30" or "上午4:30",
// each a different width and some of them a different character count. The
// column arithmetic would still be correct and the result would still look
// wrong. The formats here are therefore built by hand from TimeZone::Break and
// from fixed English name tables, exactly as the macOS original pins itself to
// en_US_POSIX for the same reason.

#include "daylist.h"

#include <algorithm>

#include "keywords.h"

namespace rc {
namespace daylist {

namespace {

// Index 0 is Sunday, matching TimeZone::Parts::weekday.
const wchar_t* const kWeekdays[] = {L"Sunday",    L"Monday",   L"Tuesday", L"Wednesday",
                                    L"Thursday",  L"Friday",   L"Saturday"};

// Index 0 is January; Parts::month is one-based, so subtract one on the way in.
const wchar_t* const kMonths[] = {L"January", L"February", L"March",     L"April",
                                  L"May",     L"June",     L"July",      L"August",
                                  L"September", L"October", L"November", L"December"};

const wchar_t* WeekdayName(int weekday) {
    if (weekday < 0 || weekday > 6) return L"";
    return kWeekdays[weekday];
}

const wchar_t* MonthName(int month) {
    if (month < 1 || month > 12) return L"";
    return kMonths[month - 1];
}

// 12-hour clock with a zero-padded hour. Midnight and midday both map to 12,
// which is the one place where the arithmetic is not simply hour % 12.
void To12Hour(int hour24, int* hour12, const wchar_t** meridiem) {
    const int h = ((hour24 % 12) + 12) % 12;
    *hour12 = (h == 0) ? 12 : h;
    *meridiem = (hour24 % 24 >= 12) ? L"PM" : L"AM";
}

std::wstring ClockString(const TimeZone& zone, Seconds t) {
    const TimeZone::Parts p = zone.Break(t);
    int hour12 = 12;
    const wchar_t* meridiem = L"AM";
    To12Hour(p.hour, &hour12, &meridiem);
    return Format(L"%02d:%02d %s", hour12, p.minute, meridiem);
}

std::wstring SeparatorText(const TimeZone& zone, Seconds t) {
    const TimeZone::Parts p = zone.Break(t);
    return Format(L"%s, %s %d", WeekdayName(p.weekday), MonthName(p.month), p.day);
}

// The end used for overlap arithmetic. A zero-length event -- a reminder, in
// practice -- would otherwise overlap nothing at all, including itself, and the
// badge would be silently wrong on exactly the rows a reminder collides with.
Seconds OverlapEnd(const CalEvent& e) {
    return (e.end > e.start + 60) ? e.end : e.start + 60;
}

}  // namespace

std::vector<CalEvent> CycleEvents(const std::vector<CalEvent>& all,
                                  Seconds now,
                                  const TimeZone& zone,
                                  const std::wstring& anchorKeyword) {
    std::vector<CalEvent> sorted = all;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const CalEvent& a, const CalEvent& b) { return a.start < b.start; });

    // Today plus a rolling twenty-four hours, not today alone. A day-only
    // fallback would hide everything past midnight while the strip -- which
    // draws a window centred on now -- was already showing it, so the dropdown
    // would contradict the thing the user just clicked on.
    auto rolling = [&sorted, now, &zone]() {
        const Seconds dayStart = zone.StartOfDay(now);
        std::vector<CalEvent> out;
        for (const CalEvent& e : sorted) {
            if (e.end > dayStart && e.start <= now + 86400) out.push_back(e);
        }
        return out;
    };

    const std::wstring needle = Normalize(anchorKeyword);
    if (needle.empty()) return rolling();

    struct Run {
        Seconds start = 0;
        Seconds end = 0;
    };

    // Consecutive anchors merge into runs. A sleep logged as four separate
    // fifteen-minute chunks by a tracker is one boundary, not four, so the
    // joining tolerance is deliberately generous.
    std::vector<Run> runs;
    for (const CalEvent& e : sorted) {
        if (!ContainsWord(Normalize(e.title), needle)) continue;
        if (!runs.empty() && e.start <= runs.back().end + kAnchorJoinTolerance) {
            if (e.end > runs.back().end) runs.back().end = e.end;
        } else {
            Run r;
            r.start = e.start;
            r.end = e.end;
            runs.push_back(r);
        }
    }
    if (runs.empty()) return rolling();

    // The last run starting at or before now. None means the day's anchor has
    // not happened yet -- the user is up before their own alarm -- and there is
    // no cycle to show.
    const Run* opener = nullptr;
    for (const Run& r : runs) {
        if (r.start > now) break;  // runs inherit the sort order of their anchors
        opener = &r;
    }
    if (!opener) return rolling();

    // With no closing anchor the cycle runs to the end of the loaded window
    // rather than to the end of the calendar day, so an unfinished night shift
    // keeps reading forward instead of stopping at an arbitrary midnight.
    const TimeZone::Parts p = zone.Break(now);
    Seconds cutoff = zone.Make(p.year, p.month, p.day + 3, 0, 0, 0);
    for (const Run& r : runs) {
        if (r.start > opener->start) {
            cutoff = r.start;
            break;
        }
    }

    std::vector<CalEvent> out;
    for (const CalEvent& e : sorted) {
        if (e.start >= opener->start && e.start <= cutoff) out.push_back(e);
    }
    return out;
}

std::vector<DayRow> BuildRows(const std::vector<CalEvent>& cycle,
                              Seconds now,
                              const TimeZone& zone) {
    std::vector<DayRow> rows;
    if (cycle.empty()) return rows;

    const Seconds today = zone.StartOfDay(now);

    bool haveLastDay = false;
    Seconds lastDay = 0;

    for (size_t i = 0; i < cycle.size(); ++i) {
        const CalEvent& ev = cycle[i];
        const Seconds day = zone.StartOfDay(ev.start);

        // A separator on the first row is only useful when it says something
        // the caption above has not already said, and the caption always names
        // today. Any other opening day does get one, because a cycle that opens
        // on yesterday's sleep is otherwise indistinguishable from one that
        // opens this morning.
        const bool needsSeparator = haveLastDay ? (day != lastDay) : (day != today);
        if (needsSeparator) {
            DayRow sep;
            sep.isSeparator = true;
            sep.separatorText = SeparatorText(zone, ev.start);
            rows.push_back(sep);
        }
        lastDay = day;
        haveLastDay = true;

        DayRow row;
        if (ev.isAllDay) {
            // No times and no duration: "All day" already says both, and a
            // duration of "24h" beside it would read as a claim about the
            // event rather than about the calendar day.
            row.time = L"All day";
        } else {
            row.time = ClockString(zone, ev.start) + L" - " + ClockString(zone, ev.end);
            row.duration = FormatDuration(ev.duration());
        }

        row.title = ev.title;
        row.category = ev.category.empty() ? std::wstring(keywords::kUncategorized) : ev.category;
        row.categoryColor = ev.color.has_value() ? ev.color.value() : keywords::kUnmatchedColor;
        row.isCurrent = (ev.start <= now && now < ev.end);
        row.isPast = (ev.end <= now);

        const Seconds endI = OverlapEnd(ev);
        int overlaps = 0;
        for (size_t j = 0; j < cycle.size(); ++j) {
            const CalEvent& other = cycle[j];
            if (OverlapEnd(other) > ev.start && other.start < endI) ++overlaps;
        }
        row.overlapCount = overlaps;

        rows.push_back(row);
    }

    if (rows.size() <= static_cast<size_t>(kMaxRows)) return rows;

    // The cap is on rows rather than on events because rows are what has to fit
    // on the screen; a cycle dense enough to need separators is exactly the one
    // that would otherwise run off the bottom of the display.
    size_t keep = static_cast<size_t>(kMaxRows) - 1;  // one is spent on the tail
    while (keep > 0 && rows[keep - 1].isSeparator) --keep;  // never end on a heading

    size_t shownEvents = 0;
    for (size_t i = 0; i < keep; ++i) {
        if (!rows[i].isSeparator) ++shownEvents;
    }

    rows.resize(keep);

    DayRow more;
    more.isMore = true;
    more.title = Format(L"... and %zu more", cycle.size() - shownEvents);
    rows.push_back(more);
    return rows;
}

std::wstring Caption(Seconds now, const TimeZone& zone, const std::wstring& sourceName) {
    const TimeZone::Parts p = zone.Break(now);

    // The middle dot is escaped rather than written literally: a literal makes
    // the separator a property of how the file happens to be saved, and menu.cpp
    // already spells the same character as \u00B7 for exactly that reason.
    std::wstring caption = Format(L"Week %d  \u00B7  %s  \u00B7  %s %d, %d  \u00B7  %s",
                                  zone.IsoWeek(now), WeekdayName(p.weekday), MonthName(p.month),
                                  p.day, p.year, sourceName.c_str());

    // The simulated marker is not appended here. It has to be drawn in a
    // different colour and weight from the rest of the caption, and a single
    // string cannot say that; see SimulatedSuffix.
    return caption;
}

std::wstring SimulatedSuffix() {
    // Debug Time is easy to leave switched on and every other reading in the
    // app then quietly lies, so the caption says so wherever the caption is --
    // and says it in red, because a dim grey warning is one nobody reads.
    if (!Clock::IsSimulating()) return std::wstring();
    return L"  \u00B7  ! Simulated !";
}

std::wstring FreshnessCaption(Seconds lastFetch, bool fetching) {
    if (fetching) return L"Refreshing...";
    if (lastFetch == 0) return L"Not read yet";

    // RealNow, not Clock::Now. How stale the data is is a fact about the
    // network and the disk; shifting the simulated clock forward a week does
    // not make the feed a week older, and reporting that it does would send
    // someone hunting a fetch bug that is not there.
    Seconds age = RealNow() - lastFetch;
    if (age < 0) age = 0;  // a clock correction backwards, not a fetch from the future

    if (age < 60) return L"Updated just now";
    if (age < 3600) {
        const Seconds minutes = age / 60;
        if (minutes <= 1) return L"Updated a minute ago";
        return Format(L"Updated %lld minutes ago", static_cast<long long>(minutes));
    }

    const TimeZone local;
    const TimeZone::Parts p = local.Break(lastFetch);
    int hour12 = 12;
    const wchar_t* meridiem = L"AM";
    To12Hour(p.hour, &hour12, &meridiem);
    return Format(L"Updated at %d:%02d:%02d %s", hour12, p.minute, p.second, meridiem);
}

}  // namespace daylist
}  // namespace rc
