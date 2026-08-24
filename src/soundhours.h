// soundhours.h — the single schedule both sound features consult.
//
// One gate, two features. Alerts and the chime never disagree about whether now
// is a reasonable time to make a noise, because there is only one answer.
//
// Windows are a *set*, not a choice: several may be ticked. Emptying the list
// is Off -- there is no separate state to get out of sync with the list.

#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "settings.h"

namespace rc {
namespace soundhours {

// The two presets, plus "all day" as an explicit choice.
extern const SoundWindow kEveningPreset;   // 11:30 AM - 4:30 AM  {690, 270}
extern const SoundWindow kDaytimePreset;   // 6:00 AM - 11:00 PM  {360, 1380}
extern const SoundWindow kAllDay;          // {0, 0}

// Both endpoints are inclusive, so a window labelled 6:00 AM - 11:00 PM does
// ring the 23:00 strike. Anything else would be a puzzle rather than a feature.
bool Contains(const SoundWindow& w, int minutesOfDay);

// False when disabled or the list is empty; true when the list holds all-day;
// otherwise true when any window contains the time.
bool Allows(Seconds t, const TimeZone& zone);

// Dedupes (first-seen order), sorts by start, and writes soundHoursOn from
// whether the list ended up empty.
void SetWindows(std::vector<SoundWindow> windows);

// While the gate is off every row reads as unticked, so a click means "turn
// this on" rather than "delete this". Removal is the explicit X.
void Toggle(const SoundWindow& w);
void Remove(const SoundWindow& w);

// Called when a sound feature is switched on. If the schedule has never been
// touched and the gate is off, opens the default window -- being silenced by a
// schedule you never set looks exactly like a bug. A deliberate later Off is
// respected, which is what `soundHoursTouched` is for.
void ArmIfUntouched();

// "11:30 AM - 4:30 AM", "All day", "Off".
std::wstring Describe(const SoundWindow& w);
std::wstring DescribeCurrent();

// Accepts "8", "8am", "6:30 PM", "18:30", "1830", "11.30pm". Returns minutes
// from midnight, or -1.
int ParseTimeOfDay(const std::wstring& text);
std::wstring FormatTimeOfDay(int minutesOfDay);

}  // namespace soundhours
}  // namespace rc
