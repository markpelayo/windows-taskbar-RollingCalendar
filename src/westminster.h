// westminster.h — the Westminster Quarters, synthesised.
//
// Nothing is bundled and nothing is downloaded. A Big Ben recording is
// copyright; the tune, written in 1793, is not. So the app builds the audio
// from sine partials at startup of each ring and plays it. A few kilobytes of
// code instead of a few megabytes of samples, and it can be pitched, timed and
// volume-adjusted without a sample library.
//
// The bell timbre is inharmonic on purpose -- the hum at 0.5 and the tierce at
// 1.19 are what make a bell sound like a bell rather than an organ, and the
// tierce being a minor third is why bells sound faintly mournful.

#pragma once

#include <string>

#include "common.h"
#include "settings.h"

namespace rc {
namespace westminster {

enum class Quarter { Past, Half, To, Hour };

void Init();
void Shutdown();

// Called once per second. Rings when the minute is a multiple of fifteen, the
// mode allows that quarter, the absolute quarter index has not been rung, and
// the second is <= 5.
//
// The index is absolute (floor(epoch / 900)) rather than a wall-clock quarter,
// so the repeated hour at a DST fall-back is not silenced. It is stamped
// *before* the five-second check, so a late tick consumes that quarter
// permanently: waking a machine at twenty past must not ring the quarter it
// slept through.
void Tick(Seconds now, const TimeZone& zone);

// Rings on demand, ignoring both the schedule and Sound Hours -- that is what
// "Hear It" is for.
void Ring(Quarter quarter, int hour12);

bool IsRinging();
void Stop();

// The device changed under us -- headphones, Bluetooth, a dock. The render
// client has to be rebuilt; playing into the old one throws.
void OnDeviceChanged();

std::wstring DescribeMode(ChimeMode mode);

}  // namespace westminster
}  // namespace rc
