// demodata.cpp — the day the app shows you before you own it.
//
// The shape of the day matters more than the contents. It opens and closes on
// Sleep so the dropdown's sleep-to-sleep cycle has two boundaries to find; it
// runs back-to-back for most of the afternoon so the gutter's chain-score tie
// break has something to prefer; and it double- and triple-books itself twice
// so the overlap badges appear without anyone having to arrange a clash.

#include "demodata.h"

#include <algorithm>

namespace rc {
namespace demodata {
namespace {

constexpr Seconds kDaySeconds = 86400;

struct Block {
    int startMinute;
    int endMinute;
    const wchar_t* title;
};

// Minutes from local midnight. The last block runs past 24:00 into the next
// day, which is the point of it: a night shift should read as one stretch.
const Block kBlocks[] = {
    {270, 690, L"Sleep"},
    {690, 720, L"Stretching | Exercise | Breakfast"},
    {720, 750, L"Read Tasks | Make a TO-DO list"},
    {750, 870, L"Focus Work | Learn"},
    {870, 900, L"Update tasks | Update the TO-DO list"},
    {900, 930, L"Weekly Planning"},
    {900, 920, L"Client Call | Acme Renewal"},
    {930, 1020, L"Focus Work | Learn"},
    {960, 990, L"Interview | Candidate Screen"},
    {975, 990, L"Standup | Team Check-in"},
    {1020, 1050, L"Power Nap"},
    {1050, 1080, L"Lunch"},
    {1080, 1140, L"Finalising Work | Learn"},
    {1140, 1200, L"Me Time | Exercise | Bath | Rest"},
    {1200, 1230, L"Reading | Self Development"},
    {1230, 1680, L"Corporate Work"},
};

}  // namespace

std::vector<CalEvent> Events(Seconds around, const TimeZone& zone) {
    const std::vector<int>& offsets = ics::DefaultDayOffsets();

    std::vector<CalEvent> events;
    events.reserve(offsets.size() * (sizeof(kBlocks) / sizeof(kBlocks[0])));

    const Seconds base = zone.StartOfDay(around);

    for (int offset : offsets) {
        // Shifted from noon, not from midnight: adding whole days to a midnight
        // lands an hour either side of it across a DST boundary, and StartOfDay
        // would then hand back the wrong date.
        const Seconds dayStart =
            zone.StartOfDay(base + static_cast<Seconds>(offset) * kDaySeconds + kDaySeconds / 2);

        for (const Block& b : kBlocks) {
            CalEvent e;
            e.title = b.title;
            // Built as startOfDay + minutes, deliberately. On a DST day the
            // wall-clock times shift by an hour rather than being re-normalised,
            // which is exactly what a real feed of floating times would do.
            e.start = dayStart + static_cast<Seconds>(b.startMinute) * 60;
            e.end = dayStart + static_cast<Seconds>(b.endMinute) * 60;
            e.isAllDay = false;
            // colour and category are left unset on purpose: the demo goes
            // through the same keyword rules a real feed does, so clearing the
            // rules turns it grey exactly as it would turn your own calendar
            // grey. No special case, so no second code path to keep working.
            events.push_back(e);
        }
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const CalEvent& a, const CalEvent& b) { return a.start < b.start; });
    return events;
}

}  // namespace demodata
}  // namespace rc
