// demodata.h — a realistic day, so the app is worth looking at before you have
// pasted a calendar into it.
//
// Titles and times only. Colour comes from the same keyword rules a real feed
// goes through, so "Clear Keyword Colors" turns the demo grey exactly as it
// would turn your own calendar grey. That is the point: the demo exercises the
// real path rather than a special case.

#pragma once

#include <vector>

#include "common.h"
#include "ics.h"

namespace rc {
namespace demodata {

// Sixteen blocks per day for the four loaded days, anchored to each day's local
// midnight. The day opens and closes on Sleep, so the dropdown's sleep-to-sleep
// cycle has something to find, and there are two deliberate collisions -- one
// two deep, one three deep -- so the overlap badges are visible without having
// to double-book yourself first.
std::vector<CalEvent> Events(Seconds around, const TimeZone& zone);

}  // namespace demodata
}  // namespace rc
