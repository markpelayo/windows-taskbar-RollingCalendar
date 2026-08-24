// ics.cpp — the RFC 5545 subset the app actually needs.
//
// Every date here is parsed digit by digit. That looks like reinventing a
// wheel until you count the work: a feed with several hundred events, re-read
// every five minutes, carries a DTSTART, usually a DTEND, often a handful of
// EXDATEs and an UNTIL, and a formatter-per-field costs an object construction
// and a locale lookup each time. Reading eight digits costs eight comparisons.
//
// The other reason is control. A lenient formatter accepts 20260230 and quietly
// hands back the 2nd of March; here the epoch is broken back down and the year,
// month and day must round-trip before the value is believed.
//
// Nothing in this file may throw or loop unboundedly on hostile input. Every
// parse either produces a value or reports failure.

#include "ics.h"

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace rc {
namespace ics {
namespace {

constexpr Seconds kDaySeconds = 86400;
constexpr Seconds kDefaultTimedDuration = 1800;  // 30 min, per the spec
constexpr Seconds kMaxDuration = 366 * kDaySeconds;
constexpr Seconds kExDateTolerance = 60;

// Caps that exist only so that a malformed or malicious feed cannot make the
// parser run for longer than a well-formed one of similar size.
constexpr size_t kMaxEvents = 20000;
constexpr size_t kMaxExDatesPerEvent = 4096;
constexpr size_t kMaxByParts = 64;
constexpr int kMaxDurationDigits = 9;

// ------------------------------------------------------------ civil calendar
//
// Days-from-civil / civil-from-days (Howard Hinnant's public-domain algorithms,
// proleptic Gregorian). Used for two things: UTC timestamps, which have no zone
// to consult, and all recurrence arithmetic, which is about calendar dates and
// must not be perturbed by a DST offset partway through the series.

int64_t DaysFromCivil(int y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (m + (m > 2 ? -3 : 9));
    const int64_t doy = (153 * mp + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t z, int* year, int* month, int* day) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    *year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    *month = static_cast<int>(m);
    *day = static_cast<int>(d);
}

// 0 = Sunday. Epoch day 0 (1 January 1970) was a Thursday.
int WeekdayFromDays(int64_t days) {
    return static_cast<int>(((days % 7) + 11) % 7);
}

int DaysInMonth(int year, int month) {
    static const int kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kLengths[month - 1];
}

// ------------------------------------------------------------------ zone cache
//
// Resolving an IANA identifier means a table lookup and a Windows zone-key
// open, so it is done once per distinct TZID rather than once per property. An
// identifier we cannot resolve reads in the *display* zone: a feed's own idea
// of local time is a better guess than the reader's machine setting.
class ZoneCache {
public:
    explicit ZoneCache(const TimeZone& display) : display_(display) {}

    const TimeZone& Get(const std::wstring& tzid) {
        if (tzid.empty()) return display_;
        auto it = map_.find(tzid);
        if (it != map_.end()) return *it->second;

        auto zone = std::make_unique<TimeZone>();
        if (!zone->SetIana(tzid)) {
            map_.emplace(tzid, &display_);
            return display_;
        }
        const TimeZone* raw = zone.get();
        owned_.push_back(std::move(zone));
        map_.emplace(tzid, raw);
        return *raw;
    }

private:
    const TimeZone& display_;
    std::map<std::wstring, const TimeZone*> map_;
    std::vector<std::unique_ptr<TimeZone>> owned_;
};

// -------------------------------------------------------------- small parsing

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// Reads exactly `count` digits starting at `i`, advancing it on success.
bool ReadDigits(const std::wstring& s, size_t* i, int count, int* out) {
    if (*i + static_cast<size_t>(count) > s.size()) return false;
    int value = 0;
    for (int n = 0; n < count; ++n) {
        const wchar_t c = s[*i + static_cast<size_t>(n)];
        if (!IsDigit(c)) return false;
        value = value * 10 + (c - L'0');
    }
    *i += static_cast<size_t>(count);
    *out = value;
    return true;
}

// A signed integer with a leading sign and at most nine digits, so no input can
// drive an overflow. Trailing rubbish rejects the whole token.
bool ParseSmallInt(const std::wstring& s, int* out) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == L'+' || s[i] == L'-')) {
        negative = (s[i] == L'-');
        ++i;
    }
    if (i >= s.size()) return false;
    int64_t value = 0;
    int digits = 0;
    for (; i < s.size(); ++i) {
        if (!IsDigit(s[i])) return false;
        if (++digits > kMaxDurationDigits) return false;
        value = value * 10 + (s[i] - L'0');
    }
    *out = static_cast<int>(negative ? -value : value);
    return true;
}

std::wstring StripQuotes(const std::wstring& s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// SUMMARY carries escapes; \n and \N are a line break, which on a single-line
// label is only ever going to be a space. An unknown escape yields the escaped
// character itself, which is what every tolerant reader does.
std::wstring UnescapeText(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        const wchar_t next = s[++i];
        switch (next) {
            case L'n':
            case L'N': out.push_back(L' '); break;
            default: out.push_back(next); break;
        }
    }
    return out;
}

// -------------------------------------------------------------- line handling

// CRLF and lone CR both become LF first, then a line opening with a space or a
// tab is a continuation of the one before it (RFC 5545 folding). Doing this in
// one pass over the body avoids holding two copies of a large feed.
std::vector<std::wstring> Unfold(const std::wstring& body) {
    std::vector<std::wstring> lines;
    std::wstring line;
    size_t i = 0;

    auto flush = [&]() {
        if (!line.empty() && (line[0] == L' ' || line[0] == L'\t') && !lines.empty()) {
            lines.back().append(line, 1, std::wstring::npos);
        } else {
            lines.push_back(line);
        }
        line.clear();
    };

    while (i < body.size()) {
        const wchar_t c = body[i];
        if (c == L'\r') {
            flush();
            ++i;
            if (i < body.size() && body[i] == L'\n') ++i;
        } else if (c == L'\n') {
            flush();
            ++i;
        } else {
            line.push_back(c);
            ++i;
        }
    }
    if (!line.empty()) flush();
    return lines;
}

struct Param {
    std::wstring key;    // lowercased
    std::wstring value;  // quotes stripped
};

struct Prop {
    std::wstring name;  // lowercased
    std::vector<Param> params;
    std::wstring value;
};

// NAME[;PARAM=VALUE]*:VALUE, with the colon and semicolons only counting
// outside a quoted parameter value (a TZID may legitimately contain both).
bool SplitProp(const std::wstring& line, Prop* out) {
    bool quoted = false;
    size_t colon = std::wstring::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if (c == L'"') {
            quoted = !quoted;
        } else if (c == L':' && !quoted) {
            colon = i;
            break;
        }
    }
    if (colon == std::wstring::npos) return false;

    out->params.clear();
    out->value = line.substr(colon + 1);

    const std::wstring head = line.substr(0, colon);
    quoted = false;
    size_t begin = 0;
    std::vector<std::wstring> parts;
    for (size_t i = 0; i <= head.size(); ++i) {
        if (i == head.size() || (head[i] == L';' && !quoted)) {
            parts.push_back(head.substr(begin, i - begin));
            begin = i + 1;
        } else if (head[i] == L'"') {
            quoted = !quoted;
        }
    }
    if (parts.empty()) return false;

    out->name = Lower(Trim(parts[0]));
    for (size_t i = 1; i < parts.size(); ++i) {
        const size_t eq = parts[i].find(L'=');
        Param p;
        if (eq == std::wstring::npos) {
            p.key = Lower(Trim(parts[i]));
        } else {
            p.key = Lower(Trim(parts[i].substr(0, eq)));
            p.value = StripQuotes(Trim(parts[i].substr(eq + 1)));
        }
        out->params.push_back(p);
    }
    return true;
}

std::wstring ParamValue(const Prop& prop, const wchar_t* key) {
    for (const Param& p : prop.params) {
        if (p.key == key) return p.value;
    }
    return std::wstring();
}

// --------------------------------------------------------------- date parsing

struct IcsDate {
    bool valid = false;
    bool allDay = false;
    bool utc = false;
    Seconds t = 0;
};

// yyyyMMdd[THHmmss][Z]. A tail after the digits we expect is ignored rather
// than rejected: some feeds append fragments there and the old DateFormatter
// path silently tolerated them.
IcsDate ParseIcsDate(const std::wstring& raw,
                     const std::wstring& tzid,
                     bool valueIsDate,
                     ZoneCache* zones) {
    IcsDate result;
    const std::wstring s = Trim(raw);

    size_t i = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!ReadDigits(s, &i, 4, &year)) return result;
    if (!ReadDigits(s, &i, 2, &month)) return result;
    if (!ReadDigits(s, &i, 2, &day)) return result;

    bool hasTime = false;
    if (i < s.size() && (s[i] == L'T' || s[i] == L't')) {
        const size_t save = ++i;
        if (ReadDigits(s, &i, 2, &hour) && ReadDigits(s, &i, 2, &minute) &&
            ReadDigits(s, &i, 2, &second)) {
            hasTime = true;
        } else {
            // A T with no usable time behind it: treat the value as date-only.
            i = save;
            hour = minute = second = 0;
        }
    }
    if (i < s.size() && (s[i] == L'Z' || s[i] == L'z')) {
        result.utc = true;
    }

    // Leap seconds are rejected outright: 23:59:60 has no epoch second to map
    // to and would otherwise silently become the next minute.
    if (month < 1 || month > 12) return result;
    if (day < 1 || day > 31) return result;
    if (hour < 0 || hour > 23) return result;
    if (minute < 0 || minute > 59) return result;
    if (second < 0 || second > 59) return result;
    if (year < 1601 || year > 9999) return result;

    result.allDay = valueIsDate || (!result.utc && !hasTime);

    if (result.utc) {
        const int64_t days = DaysFromCivil(year, month, day);
        int cy = 0, cm = 0, cd = 0;
        CivilFromDays(days, &cy, &cm, &cd);
        if (cy != year || cm != month || cd != day) return result;  // e.g. 30 Feb
        result.t = days * kDaySeconds + hour * 3600 + minute * 60 + second;
    } else {
        const TimeZone& zone = zones->Get(tzid);
        const Seconds t = zone.Make(year, month, day, hour, minute, second);
        const TimeZone::Parts back = zone.Break(t);
        if (back.year != year || back.month != month || back.day != day) return result;
        result.t = t;
    }

    result.valid = true;
    return result;
}

// ISO 8601 durations. M is ambiguous by design in this format: minutes after a
// T, months before one. Months are taken as 30 days, which is what the original
// did and is only ever seen in feeds that mean "about a month".
bool ParseDuration(const std::wstring& raw, Seconds* out) {
    const std::wstring s = Trim(raw);
    size_t i = 0;
    if (i < s.size() && s[i] == L'+') ++i;
    if (i < s.size() && s[i] == L'-') return false;  // a negative span has no meaning here
    if (i >= s.size() || (s[i] != L'P' && s[i] != L'p')) return false;
    ++i;

    bool inTime = false;
    bool sawUnit = false;
    int64_t total = 0;

    while (i < s.size()) {
        if (s[i] == L'T' || s[i] == L't') {
            inTime = true;
            ++i;
            continue;
        }
        if (!IsDigit(s[i])) return false;

        int64_t value = 0;
        int digits = 0;
        while (i < s.size() && IsDigit(s[i])) {
            if (++digits > kMaxDurationDigits) return false;
            value = value * 10 + (s[i] - L'0');
            ++i;
        }
        if (i >= s.size()) return false;

        int64_t unit = 0;
        switch (s[i]) {
            case L'W': case L'w': unit = 604800; break;
            case L'D': case L'd': unit = kDaySeconds; break;
            case L'H': case L'h': unit = 3600; break;
            case L'M': case L'm': unit = inTime ? 60 : 2592000; break;
            case L'S': case L's': unit = 1; break;
            default: return false;
        }
        ++i;

        if (value > kMaxDuration) return false;
        total += value * unit;
        if (total < 0 || total > kMaxDuration) return false;  // an unbounded span
        sawUnit = true;                                       // would drag `end` to infinity
    }

    if (!sawUnit) return false;
    *out = total;
    return true;
}

// ------------------------------------------------------------------ recurrence

struct ByDay {
    int nth = 0;  // 0 = every such weekday in the period
    int weekday = 0;
};

struct RRule {
    std::wstring freq;  // lowercased
    int interval = 1;
    std::vector<ByDay> byDay;
    std::vector<int> byMonthDay;
    std::wstring until;
    bool hasUntil = false;
    int count = 0;
    bool hasCount = false;
};

bool ParseWeekday(const std::wstring& code, int* out) {
    static const wchar_t* kCodes[7] = {L"su", L"mo", L"tu", L"we", L"th", L"fr", L"sa"};
    const std::wstring lower = Lower(code);
    for (int i = 0; i < 7; ++i) {
        if (lower == kCodes[i]) {
            *out = i;
            return true;
        }
    }
    return false;
}

bool ParseByDay(const std::wstring& token, ByDay* out) {
    const std::wstring t = Trim(token);
    if (t.size() < 2) return false;
    const size_t split = t.size() - 2;
    int weekday = 0;
    if (!ParseWeekday(t.substr(split), &weekday)) return false;
    int nth = 0;
    if (split > 0 && !ParseSmallInt(t.substr(0, split), &nth)) return false;
    if (nth < -53 || nth > 53) return false;
    out->nth = nth;
    out->weekday = weekday;
    return true;
}

RRule ParseRRule(const std::wstring& value) {
    RRule rule;
    for (const std::wstring& part : Split(value, L';')) {
        const size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = Lower(Trim(part.substr(0, eq)));
        const std::wstring val = Trim(part.substr(eq + 1));

        if (key == L"freq") {
            rule.freq = Lower(val);
        } else if (key == L"interval") {
            int n = 0;
            if (ParseSmallInt(val, &n)) rule.interval = (n < 1) ? 1 : n;
        } else if (key == L"count") {
            int n = 0;
            if (ParseSmallInt(val, &n) && n >= 0) {
                rule.count = n;
                rule.hasCount = true;
            }
        } else if (key == L"until") {
            rule.until = val;
            rule.hasUntil = true;
        } else if (key == L"byday") {
            for (const std::wstring& token : Split(val, L',')) {
                if (rule.byDay.size() >= kMaxByParts) break;
                ByDay bd;
                if (ParseByDay(token, &bd)) rule.byDay.push_back(bd);
            }
        } else if (key == L"bymonthday") {
            for (const std::wstring& token : Split(val, L',')) {
                if (rule.byMonthDay.size() >= kMaxByParts) break;
                int n = 0;
                if (ParseSmallInt(Trim(token), &n) && n != 0 && n >= -31 && n <= 31) {
                    rule.byMonthDay.push_back(n);
                }
            }
        }
    }
    return rule;
}

bool MatchesWeekday(const std::vector<ByDay>& list, int weekday) {
    for (const ByDay& bd : list) {
        if (bd.weekday == weekday) return true;
    }
    return false;
}

bool MatchesNthWeekday(const std::vector<ByDay>& list, int weekday, int day, int daysInMonth) {
    for (const ByDay& bd : list) {
        if (bd.weekday != weekday) continue;
        if (bd.nth == 0) return true;
        if (bd.nth > 0 && ((day - 1) / 7) + 1 == bd.nth) return true;
        if (bd.nth < 0 && ((daysInMonth - day) / 7) + 1 == -bd.nth) return true;
    }
    return false;
}

// ------------------------------------------------------------------- raw event

struct RawEvent {
    std::wstring uid;
    std::wstring summary;
    std::wstring status;  // lowercased
    std::wstring tzid;    // the DTSTART zone, which is the event's own zone
    IcsDate start;
    IcsDate end;
    IcsDate recurrenceId;
    Seconds duration = 0;
    bool hasDuration = false;
    bool hasRecurrenceId = false;
    std::vector<Seconds> exDates;
    std::wstring rrule;
};

// One requested day, held as both an instant range (for filtering) and a civil
// date (for recurrence arithmetic, which must not care about DST).
struct Day {
    Seconds start = 0;
    Seconds end = 0;
    int year = 1970, month = 1, day = 1;
    int64_t days = 0;
    int weekday = 4;
};

Seconds EndOf(const RawEvent& ev) {
    if (ev.end.valid) return ev.end.t;
    if (ev.hasDuration) return ev.start.t + ev.duration;
    return ev.start.t + (ev.start.allDay ? kDaySeconds : kDefaultTimedDuration);
}

// The dropdown and the strip both cope with a zero-length block; they do not
// cope with one that ends before it starts.
Seconds ClampEnd(Seconds start, Seconds end) { return (end < start) ? start : end; }

std::wstring OverrideKey(const std::wstring& uid, Seconds originalStart) {
    return Format(L"%s@%lld", uid.c_str(), static_cast<long long>(originalStart));
}

void AppendOccurrence(std::vector<CalEvent>* out,
                      const std::wstring& title,
                      Seconds start,
                      Seconds end,
                      bool allDay) {
    if (out->size() >= kMaxEvents) return;
    CalEvent e;
    e.title = title;
    e.start = start;
    e.end = ClampEnd(start, end);
    e.isAllDay = allDay;
    out->push_back(e);
}

void ExpandRecurring(const RawEvent& master,
                     const std::vector<Day>& days,
                     const std::unordered_map<std::wstring, const RawEvent*>& overrides,
                     ZoneCache* zones,
                     std::vector<CalEvent>* out) {
    const RRule rule = ParseRRule(master.rrule);
    if (rule.freq.empty()) return;

    const bool daily = (rule.freq == L"daily");
    const bool weekly = (rule.freq == L"weekly");
    const bool monthly = (rule.freq == L"monthly");
    const bool yearly = (rule.freq == L"yearly");
    if (!daily && !weekly && !monthly && !yearly) return;  // SECONDLY and friends: nothing

    const TimeZone& evZone = zones->Get(master.tzid);
    const TimeZone::Parts from = evZone.Break(master.start.t);
    const int64_t startDays = DaysFromCivil(from.year, from.month, from.day);
    const int startWeekday = WeekdayFromDays(startDays);
    const Seconds masterDuration = ClampEnd(master.start.t, EndOf(master)) - master.start.t;

    // UNTIL is read in the event's own zone. A date-only UNTIL interpreted in
    // the reader's zone gains or loses an occurrence at the tail of every
    // bounded series, which is exactly the kind of off-by-one nobody reports.
    Seconds until = 0;
    bool hasUntil = false;
    if (rule.hasUntil) {
        const IcsDate parsed = ParseIcsDate(rule.until, master.tzid, false, zones);
        if (parsed.valid) {
            until = parsed.t;
            hasUntil = true;
        }
    }

    for (const Day& day : days) {
        const int64_t dayDiff = day.days - startDays;
        if (dayDiff < 0) continue;

        bool matches = false;
        int64_t occurrenceIndex = 0;

        if (daily) {
            if (dayDiff % rule.interval != 0) continue;
            matches = rule.byDay.empty() || MatchesWeekday(rule.byDay, day.weekday);
            occurrenceIndex = dayDiff / rule.interval;
        } else if (weekly) {
            // Weeks start on Sunday, matching the original's calendar.
            const int64_t sundayOfStart = startDays - startWeekday;
            const int64_t sundayOfDay = day.days - day.weekday;
            const int64_t weekDiff = (sundayOfDay - sundayOfStart) / 7;
            if (weekDiff < 0 || weekDiff % rule.interval != 0) continue;
            matches = rule.byDay.empty() ? (day.weekday == startWeekday)
                                         : MatchesWeekday(rule.byDay, day.weekday);
            // Approximate for WEEKLY with several BYDAY values: the index
            // assumes every listed weekday produced an occurrence, including in
            // the first and last weeks where DTSTART or UNTIL may cut it short.
            const int64_t perWeek = rule.byDay.empty() ? 1 : static_cast<int64_t>(rule.byDay.size());
            occurrenceIndex = (weekDiff / rule.interval) * perWeek;
        } else if (monthly) {
            const int64_t monthDiff =
                static_cast<int64_t>(day.year - from.year) * 12 + (day.month - from.month);
            if (monthDiff < 0 || monthDiff % rule.interval != 0) continue;
            if (!rule.byMonthDay.empty()) {
                const int dim = DaysInMonth(day.year, day.month);
                for (int n : rule.byMonthDay) {
                    const int target = (n > 0) ? n : (dim + n + 1);
                    if (target == day.day) {
                        matches = true;
                        break;
                    }
                }
            } else if (!rule.byDay.empty()) {
                matches = MatchesNthWeekday(rule.byDay, day.weekday, day.day,
                                            DaysInMonth(day.year, day.month));
            } else {
                matches = (day.day == from.day);
            }
            occurrenceIndex = monthDiff / rule.interval;
        } else {  // yearly
            const int64_t yearDiff = day.year - from.year;
            if (yearDiff < 0 || yearDiff % rule.interval != 0) continue;
            matches = (day.month == from.month && day.day == from.day);
            occurrenceIndex = yearDiff / rule.interval;
        }

        if (!matches) continue;
        if (rule.hasCount && occurrenceIndex >= rule.count) continue;

        // The original time of day, re-applied to the target date in the
        // event's own zone: a 09:00 standup stays at 09:00 across a DST change.
        const Seconds occStart =
            evZone.Make(day.year, day.month, day.day, from.hour, from.minute, from.second);
        if (hasUntil && occStart > until) continue;

        bool excluded = false;
        for (Seconds ex : master.exDates) {
            const Seconds delta = occStart - ex;
            if (delta <= kExDateTolerance && delta >= -kExDateTolerance) {
                excluded = true;
                break;
            }
        }
        if (excluded) continue;

        std::wstring title = master.summary;
        Seconds start = occStart;
        Seconds end = occStart + masterDuration;

        if (!master.uid.empty()) {
            const auto it = overrides.find(OverrideKey(master.uid, occStart));
            if (it != overrides.end()) {
                const RawEvent& ov = *it->second;
                if (ov.status == L"cancelled") continue;
                if (!ov.summary.empty()) title = ov.summary;
                if (ov.start.valid) {
                    start = ov.start.t;
                    end = ov.end.valid ? ov.end.t
                                       : (ov.hasDuration ? start + ov.duration
                                                         : start + masterDuration);
                }
            }
        }

        AppendOccurrence(out, title, start, end, master.start.allDay);
    }
}

}  // namespace

bool LooksLikeCalendar(const std::wstring& body) {
    return body.find(L"BEGIN:VCALENDAR") != std::wstring::npos;
}

std::vector<CalEvent> Parse(const std::wstring& body,
                            Seconds now,
                            const TimeZone& zone,
                            const std::vector<int>& dayOffsets) {
    std::vector<CalEvent> events;
    if (!LooksLikeCalendar(body) || dayOffsets.empty()) return events;

    // Each requested day is resolved through StartOfDay from *noon* of the
    // shifted instant. Adding whole days to a midnight lands at 23:00 or 01:00
    // across a DST boundary; from noon the arithmetic cannot fall into the
    // neighbouring date.
    const Seconds base = zone.StartOfDay(now);
    std::vector<Day> days;
    days.reserve(dayOffsets.size());
    for (int offset : dayOffsets) {
        Day d;
        d.start = zone.StartOfDay(base + static_cast<Seconds>(offset) * kDaySeconds + kDaySeconds / 2);
        d.end = zone.StartOfDay(d.start + kDaySeconds + kDaySeconds / 2);
        const TimeZone::Parts p = zone.Break(d.start + kDaySeconds / 2);
        d.year = p.year;
        d.month = p.month;
        d.day = p.day;
        d.days = DaysFromCivil(p.year, p.month, p.day);
        d.weekday = WeekdayFromDays(d.days);
        days.push_back(d);
    }

    ZoneCache zones(zone);

    std::vector<RawEvent> masters;
    std::vector<RawEvent> overrideEvents;
    RawEvent current;
    bool inEvent = false;

    for (const std::wstring& line : Unfold(body)) {
        if (line.empty()) continue;

        if (!inEvent) {
            if (Lower(Trim(line)) == L"begin:vevent") {
                current = RawEvent();
                inEvent = true;
            }
            continue;
        }

        if (Lower(Trim(line)) == L"end:vevent") {
            inEvent = false;
            if (current.status != L"cancelled" || current.hasRecurrenceId) {
                if (current.hasRecurrenceId) {
                    if (overrideEvents.size() < kMaxEvents) overrideEvents.push_back(current);
                } else if (current.start.valid && masters.size() < kMaxEvents) {
                    masters.push_back(current);
                }
            }
            continue;
        }

        Prop prop;
        if (!SplitProp(line, &prop)) continue;

        if (prop.name == L"summary") {
            current.summary = Trim(UnescapeText(prop.value));
        } else if (prop.name == L"uid") {
            current.uid = Trim(prop.value);
        } else if (prop.name == L"status") {
            current.status = Lower(Trim(prop.value));
        } else if (prop.name == L"dtstart") {
            current.tzid = ParamValue(prop, L"tzid");
            current.start = ParseIcsDate(prop.value, current.tzid,
                                         Lower(ParamValue(prop, L"value")) == L"date", &zones);
        } else if (prop.name == L"dtend") {
            current.end = ParseIcsDate(prop.value, ParamValue(prop, L"tzid"),
                                       Lower(ParamValue(prop, L"value")) == L"date", &zones);
        } else if (prop.name == L"duration") {
            Seconds d = 0;
            if (ParseDuration(prop.value, &d)) {
                current.duration = d;
                current.hasDuration = true;
            }
        } else if (prop.name == L"recurrence-id") {
            current.recurrenceId = ParseIcsDate(prop.value, ParamValue(prop, L"tzid"),
                                                Lower(ParamValue(prop, L"value")) == L"date",
                                                &zones);
            current.hasRecurrenceId = current.recurrenceId.valid;
        } else if (prop.name == L"exdate") {
            const std::wstring tzid = ParamValue(prop, L"tzid");
            const bool asDate = Lower(ParamValue(prop, L"value")) == L"date";
            for (const std::wstring& token : Split(prop.value, L',')) {
                if (current.exDates.size() >= kMaxExDatesPerEvent) break;
                const IcsDate d = ParseIcsDate(token, tzid, asDate, &zones);
                if (d.valid) current.exDates.push_back(d.t);
            }
        } else if (prop.name == L"rrule") {
            current.rrule = Trim(prop.value);
        }
        // Everything else — LOCATION, DESCRIPTION, ATTENDEE, alarms — is of no
        // use to a strip that draws a title and two timestamps.
    }

    // Overrides are keyed by the start they replace, so an expansion can look
    // one up without scanning.
    std::unordered_map<std::wstring, const RawEvent*> overrides;
    for (const RawEvent& ov : overrideEvents) {
        if (ov.uid.empty() || !ov.recurrenceId.valid) continue;
        overrides.emplace(OverrideKey(ov.uid, ov.recurrenceId.t), &ov);
    }

    std::vector<CalEvent> collected;
    for (const RawEvent& ev : masters) {
        if (!ev.rrule.empty()) {
            ExpandRecurring(ev, days, overrides, &zones, &collected);
            continue;
        }

        // Non-recurring events are filtered here rather than afterwards, so a
        // calendar with ten years of history costs the same to refresh as one
        // holding a single week.
        const Seconds start = ev.start.t;
        const Seconds end = ClampEnd(start, EndOf(ev));
        for (const Day& day : days) {
            if (end > day.start && start < day.end) {
                AppendOccurrence(&collected, ev.summary, start, end, ev.start.allDay);
                break;
            }
        }
    }

    // The same occurrence can arrive twice — two VEVENTs sharing a UID, an
    // override landing back on its original slot, two requested days claiming
    // the same instant. Identity is what the user would see: title and span.
    std::unordered_set<std::wstring> seen;
    events.reserve(collected.size());
    for (const CalEvent& e : collected) {
        const std::wstring key = Format(L"%s|%lld|%lld", e.title.c_str(),
                                        static_cast<long long>(e.start),
                                        static_cast<long long>(e.end));
        if (!seen.insert(key).second) continue;
        events.push_back(e);
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const CalEvent& a, const CalEvent& b) { return a.start < b.start; });
    return events;
}

const std::vector<int>& DefaultDayOffsets() {
    // Four days, not one. The dropdown lists a sleep-to-sleep cycle rather than
    // a midnight-to-midnight day, so it has to be able to find both boundaries:
    // the anchor that opened the current stretch may lie in yesterday, and the
    // one that closes it may lie past tomorrow's midnight on a late night.
    static const std::vector<int> kOffsets = {-1, 0, 1, 2};
    return kOffsets;
}

}  // namespace ics
}  // namespace rc
