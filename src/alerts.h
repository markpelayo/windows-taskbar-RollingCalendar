// alerts.h — telling you a block is about to start.
//
// Lead times are a *set*, not a choice: ten minutes before and one minute
// before are both useful and are not alternatives.
//
// The rule that matters most is the lateness rule. An alert more than thirty
// seconds late -- the machine was asleep, the app had just launched -- is
// marked as fired and silently skipped, because announcing "ten minutes before
// Focus Work" when there are three minutes left is not a late alert, it is a
// wrong one.

#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "ics.h"

namespace rc {
namespace alerts {

void Init();
void Shutdown();

// Called once per second with every loaded event -- alerts see the whole day,
// not just the dropdown's cycle.
void Tick(const std::vector<CalEvent>& allEvents, Seconds now, const TimeZone& zone);

// Fires one now regardless of Sound Hours, which is the only sensible
// behaviour for a button labelled "Test Alert Now".
void TestNow();

// Changing the lead set clears the fired map: the user has just told us the
// rules changed, and honouring the old bookkeeping would swallow the first
// alert under the new one.
void ResetFiredMap();

// ---- lead times -------------------------------------------------------
// Presets: 0 (when it starts), 60, 300, 600. Custom 0.25-120 minutes.
// Stored deduped and sorted descending. Zero is legitimate; only negatives are
// dropped.
void SetLeads(std::vector<int> seconds);
std::wstring DescribeLeads();
std::wstring LeadPhrase(int seconds);   // "10 minutes", "1 minute"

// ---- sound ------------------------------------------------------------
// The installed .wav files under %WINDIR%\Media, quietest-sounding first, plus
// anything the user has imported into %APPDATA%\RollingCalendar\Sounds.
std::vector<std::wstring> AvailableSounds();
void RefreshSounds();
void PreviewSound(const std::wstring& name);
bool ImportSound(HWND owner);   // file dialog, copies into the app's folder

// ---- speech -----------------------------------------------------------
// SAPI 5 voices. Sound and speech are mutually exclusive: choosing one turns
// the other off, because two things talking over each other is not twice as
// useful.
struct Voice {
    std::wstring id;
    std::wstring label;   // "British - male - George"
};
std::vector<Voice> AvailableVoices();
void RefreshVoices();
void Speak(const std::wstring& text);

// The name announced is the part of the title before the first '|', because a
// bar reads aloud as an awkward pause. Simultaneous blocks are announced in one
// sentence: "A", "A and B", "A, B and C" -- no Oxford comma.
std::wstring SpeechText(const std::vector<std::wstring>& titles, int leadSeconds);

// ---- categories -------------------------------------------------------
// An empty set means every category, and is stored that way, so a category
// added later is included rather than silently excluded. The first click on a
// category seeds the set with everything and removes the clicked one:
// "everything but this" is what people mean the first time.
void ToggleCategory(const std::wstring& category);
bool CategoryAllowed(const std::wstring& category);
std::wstring DescribeCategories();

constexpr int kLatenessGrace = 30;    // seconds

}  // namespace alerts
}  // namespace rc
