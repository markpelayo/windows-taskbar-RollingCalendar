// soundhours.cpp — the one gate both sound features consult.
//
// See soundhours.h for the contract. Two decisions drive most of the code
// below: both endpoints of a window are inclusive, and the enabled flag is
// derived from the list rather than stored beside it. The second one is the
// important one -- a separate on/off bool and a list of windows are two pieces
// of state that can disagree, and every implementation that keeps both
// eventually ships a bug where the gate says Off while a window sits ticked.

#include "soundhours.h"

#include <algorithm>

namespace rc {
namespace soundhours {

const SoundWindow kEveningPreset{690, 270};    // 11:30 AM - 4:30 AM
const SoundWindow kDaytimePreset{360, 1380};   // 6:00 AM - 11:00 PM
const SoundWindow kAllDay{0, 0};

namespace {

// A run of decimal digits and nothing else. Returns -1 for empty or for any
// stray character, so a typo is rejected rather than silently truncated.
int ParseDigits(const std::wstring& s) {
    if (s.empty() || s.size() > 4) return -1;
    int value = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return -1;
        value = value * 10 + (c - L'0');
    }
    return value;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool Contains(const SoundWindow& w, int minutesOfDay) {
    // All day is {0, 0}, which also wraps, so it would fall out of the general
    // case anyway. It is spelled out because relying on that coincidence is how
    // someone later "tidies" the wrap test and silences the app.
    if (w.isAllDay()) return true;

    if (w.wrapsMidnight()) {
        // 11:30 PM - 4:30 AM is two runs, one either side of midnight.
        return minutesOfDay >= w.startMinutes || minutesOfDay <= w.endMinutes;
    }
    return minutesOfDay >= w.startMinutes && minutesOfDay <= w.endMinutes;
}

bool Allows(Seconds t, const TimeZone& zone) {
    const Settings& cfg = Cfg();
    if (!cfg.soundHoursOn) return false;
    if (cfg.soundHours.empty()) return false;

    for (const SoundWindow& w : cfg.soundHours) {
        if (w.isAllDay()) return true;
    }

    const TimeZone::Parts p = zone.Break(t);
    const int minutesOfDay = p.hour * 60 + p.minute;
    for (const SoundWindow& w : cfg.soundHours) {
        if (Contains(w, minutesOfDay)) return true;
    }
    return false;
}

void SetWindows(std::vector<SoundWindow> windows) {
    // Dedupe in first-seen order before sorting, so a duplicate added from the
    // custom dialog does not quietly displace the preset the user already had.
    std::vector<SoundWindow> unique;
    unique.reserve(windows.size());
    for (const SoundWindow& w : windows) {
        if (std::find(unique.begin(), unique.end(), w) == unique.end()) unique.push_back(w);
    }

    std::stable_sort(unique.begin(), unique.end(),
                     [](const SoundWindow& a, const SoundWindow& b) {
                         return a.startMinutes < b.startMinutes;
                     });

    Settings& cfg = Cfg();
    cfg.soundHours = std::move(unique);
    cfg.soundHoursOn = !cfg.soundHours.empty();
    cfg.Save();
}

void Toggle(const SoundWindow& w) {
    Settings& cfg = Cfg();
    std::vector<SoundWindow> list = cfg.soundHours;
    const auto it = std::find(list.begin(), list.end(), w);

    // While the gate is off every row in the menu draws unticked, because that
    // is the honest picture: none of them are in force. A click on an unticked
    // row can only mean "turn this on". Deleting the window the user just
    // clicked -- which is what a symmetrical toggle would do when the row is
    // already in the list -- would make the row vanish instead of tick.
    const bool gateOff = !cfg.soundHoursOn || list.empty();
    if (gateOff) {
        if (it == list.end()) list.push_back(w);
    } else if (it != list.end()) {
        list.erase(it);
    } else {
        list.push_back(w);
    }

    cfg.soundHoursTouched = true;
    SetWindows(std::move(list));
}

void Remove(const SoundWindow& w) {
    Settings& cfg = Cfg();
    std::vector<SoundWindow> list = cfg.soundHours;
    list.erase(std::remove(list.begin(), list.end(), w), list.end());
    cfg.soundHoursTouched = true;
    SetWindows(std::move(list));
}

void ArmIfUntouched() {
    Settings& cfg = Cfg();

    // A deliberate Off is respected. `soundHoursTouched` is the whole reason
    // this can tell the two cases apart: a schedule the user has never opened,
    // and a schedule the user has emptied on purpose.
    if (cfg.soundHoursTouched) return;
    if (cfg.soundHoursOn && !cfg.soundHours.empty()) return;

    cfg.soundHours.assign(1, kEveningPreset);
    cfg.soundHoursOn = true;
    cfg.Save();
    // Deliberately not marking it touched: the app opened this window, not the
    // user, so the next genuine choice still counts as the first one.
}

std::wstring Describe(const SoundWindow& w) {
    if (w.isAllDay()) return L"All day";
    // En dash, matching the rest of the menu. Built from its code point rather
    // than typed literally so the source stays plain ASCII and does not depend
    // on the compiler guessing the file's encoding correctly.
    static const wchar_t kEnDash[] = {L' ', 0x2013, L' ', 0};
    return FormatTimeOfDay(w.startMinutes) + kEnDash + FormatTimeOfDay(w.endMinutes);
}

std::wstring DescribeCurrent() {
    const Settings& cfg = Cfg();
    if (!cfg.soundHoursOn || cfg.soundHours.empty()) return L"Off";

    for (const SoundWindow& w : cfg.soundHours) {
        if (w.isAllDay()) return L"All day";
    }

    std::wstring out;
    for (const SoundWindow& w : cfg.soundHours) {
        if (!out.empty()) out += L", ";
        out += Describe(w);
    }
    return out;
}

int ParseTimeOfDay(const std::wstring& text) {
    std::wstring s = Lower(Trim(text));
    if (s.empty()) return -1;

    // Longest suffixes first: "8p.m." must not be read as ending in "m".
    static const wchar_t* const kMeridiems[] = {L"a.m.", L"p.m.", L"am", L"pm"};
    bool isPm = false;
    bool hasMeridiem = false;
    for (const wchar_t* suffix : kMeridiems) {
        const std::wstring suf(suffix);
        if (EndsWith(s, suf)) {
            isPm = (suf[0] == L'p');
            hasMeridiem = true;
            s.erase(s.size() - suf.size());
            break;
        }
    }
    s = Trim(s);
    if (s.empty()) return -1;

    // "11.30pm" is how a good many people write it; a full stop here can only
    // be a separator, because a fractional hour is not a thing anyone types.
    std::replace(s.begin(), s.end(), L'.', L':');

    int hour = -1;
    int minute = 0;
    const size_t colon = s.find(L':');
    if (colon != std::wstring::npos) {
        hour = ParseDigits(s.substr(0, colon));
        minute = ParseDigits(s.substr(colon + 1));
    } else if (s.size() == 3 || s.size() == 4) {
        const int packed = ParseDigits(s);
        if (packed < 0) return -1;
        hour = packed / 100;
        minute = packed % 100;
    } else if (s.size() == 1 || s.size() == 2) {
        hour = ParseDigits(s);
        minute = 0;
    }

    if (hour < 0 || minute < 0) return -1;

    if (hasMeridiem) {
        if (isPm && hour != 12) hour += 12;
        if (!isPm && hour == 12) hour = 0;   // 12 AM is midnight, not noon
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
    return hour * 60 + minute;
}

std::wstring FormatTimeOfDay(int minutesOfDay) {
    if (minutesOfDay < 0 || minutesOfDay > 24 * 60) return std::wstring();

    const int hour24 = (minutesOfDay / 60) % 24;
    const int minute = minutesOfDay % 60;
    const int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
    return Format(L"%d:%02d %s", hour12, minute, hour24 < 12 ? L"AM" : L"PM");
}

}  // namespace soundhours
}  // namespace rc
