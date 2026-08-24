// tzmap.cpp — the CLDR windowsZones subset.
//
// Rows are taken from the CLDR supplemental data, territory "001" (the default
// IANA zone for each Windows zone) read in reverse. Several IANA zones map to
// the same Windows key, which is expected: Windows has one zone per offset-plus-
// rules combination, IANA has one per political history, so Toronto and New York
// are both "Eastern Standard Time" even though their pre-1970 records differ.
// The app only ever asks about the present, so the collapse is harmless.
//
// Legacy aliases (Asia/Calcutta, Europe/Kiev, America/Buenos_Aires) are listed
// alongside their modern spellings. Exchange and older Google exports still
// emit them, and a feed that names a zone we silently reject is worse than one
// we translate approximately.

#include "tzmap.h"

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <cwchar>

namespace rc {
namespace tzmap {
namespace {

struct Row {
    const wchar_t* iana;
    const wchar_t* windows;
};

// Not sorted, and deliberately so: a linear scan over a hundred rows costs
// microseconds and happens once per feed at parse time, whereas a sorted table
// has to *stay* sorted through every future edit, and nothing in the build
// checks that it has.
constexpr Row kRows[] = {
    // ------------------------------------------------------------- Americas
    {L"America/Anchorage",                L"Alaskan Standard Time"},
    {L"America/Bogota",                   L"SA Pacific Standard Time"},
    {L"America/Buenos_Aires",             L"Argentina Standard Time"},  // legacy
    {L"America/Argentina/Buenos_Aires",   L"Argentina Standard Time"},
    {L"America/Caracas",                  L"Venezuela Standard Time"},
    {L"America/Chicago",                  L"Central Standard Time"},
    {L"America/Denver",                   L"Mountain Standard Time"},
    {L"America/Edmonton",                 L"Mountain Standard Time"},
    {L"America/Halifax",                  L"Atlantic Standard Time"},
    {L"America/Lima",                     L"SA Pacific Standard Time"},
    {L"America/Los_Angeles",              L"Pacific Standard Time"},
    {L"America/Mexico_City",              L"Central Standard Time (Mexico)"},
    {L"America/New_York",                 L"Eastern Standard Time"},
    {L"America/Phoenix",                  L"US Mountain Standard Time"},
    {L"America/Santiago",                 L"Pacific SA Standard Time"},
    {L"America/Sao_Paulo",                L"E. South America Standard Time"},
    {L"America/St_Johns",                 L"Newfoundland Standard Time"},
    {L"America/Toronto",                  L"Eastern Standard Time"},
    {L"America/Vancouver",                L"Pacific Standard Time"},
    {L"America/Winnipeg",                 L"Central Standard Time"},
    {L"Pacific/Honolulu",                 L"Hawaiian Standard Time"},

    // --------------------------------------------------------------- Europe
    {L"Europe/Amsterdam",                 L"W. Europe Standard Time"},
    {L"Europe/Athens",                    L"GTB Standard Time"},
    {L"Europe/Berlin",                    L"W. Europe Standard Time"},
    {L"Europe/Brussels",                  L"Romance Standard Time"},
    {L"Europe/Bucharest",                 L"GTB Standard Time"},
    {L"Europe/Budapest",                  L"Central Europe Standard Time"},
    {L"Europe/Copenhagen",                L"Romance Standard Time"},
    {L"Europe/Dublin",                    L"GMT Standard Time"},
    {L"Europe/Helsinki",                  L"FLE Standard Time"},
    {L"Europe/Istanbul",                  L"Turkey Standard Time"},
    {L"Europe/Kiev",                      L"FLE Standard Time"},  // legacy
    {L"Europe/Kyiv",                      L"FLE Standard Time"},
    {L"Europe/Lisbon",                    L"GMT Standard Time"},
    {L"Europe/London",                    L"GMT Standard Time"},
    {L"Europe/Madrid",                    L"Romance Standard Time"},
    {L"Europe/Moscow",                    L"Russian Standard Time"},
    {L"Europe/Oslo",                      L"W. Europe Standard Time"},
    {L"Europe/Paris",                     L"Romance Standard Time"},
    {L"Europe/Prague",                    L"Central Europe Standard Time"},
    {L"Europe/Rome",                      L"W. Europe Standard Time"},
    {L"Europe/Stockholm",                 L"W. Europe Standard Time"},
    {L"Europe/Vienna",                    L"W. Europe Standard Time"},
    {L"Europe/Warsaw",                    L"Central European Standard Time"},
    {L"Europe/Zurich",                    L"W. Europe Standard Time"},

    // ----------------------------------------------------------------- Asia
    {L"Asia/Bangkok",                     L"SE Asia Standard Time"},
    {L"Asia/Calcutta",                    L"India Standard Time"},  // legacy
    {L"Asia/Dhaka",                       L"Bangladesh Standard Time"},
    {L"Asia/Dubai",                       L"Arabian Standard Time"},
    {L"Asia/Ho_Chi_Minh",                 L"SE Asia Standard Time"},
    {L"Asia/Hong_Kong",                   L"China Standard Time"},
    {L"Asia/Jakarta",                     L"SE Asia Standard Time"},
    {L"Asia/Jerusalem",                   L"Israel Standard Time"},
    {L"Asia/Karachi",                     L"Pakistan Standard Time"},
    {L"Asia/Kathmandu",                   L"Nepal Standard Time"},
    {L"Asia/Kolkata",                     L"India Standard Time"},
    {L"Asia/Kuala_Lumpur",                L"Singapore Standard Time"},
    {L"Asia/Manila",                      L"Singapore Standard Time"},
    {L"Asia/Saigon",                      L"SE Asia Standard Time"},  // legacy
    {L"Asia/Seoul",                       L"Korea Standard Time"},
    {L"Asia/Shanghai",                    L"China Standard Time"},
    {L"Asia/Singapore",                   L"Singapore Standard Time"},
    {L"Asia/Taipei",                      L"Taipei Standard Time"},
    {L"Asia/Tokyo",                       L"Tokyo Standard Time"},

    // -------------------------------------------------------------- Oceania
    {L"Australia/Adelaide",               L"Cen. Australia Standard Time"},
    {L"Australia/Brisbane",               L"E. Australia Standard Time"},
    {L"Australia/Darwin",                 L"AUS Central Standard Time"},
    {L"Australia/Hobart",                 L"Tasmania Standard Time"},
    {L"Australia/Melbourne",              L"AUS Eastern Standard Time"},
    {L"Australia/Perth",                  L"W. Australia Standard Time"},
    {L"Australia/Sydney",                 L"AUS Eastern Standard Time"},
    {L"Pacific/Auckland",                 L"New Zealand Standard Time"},
    {L"Pacific/Fiji",                     L"Fiji Standard Time"},

    // --------------------------------------------------------------- Africa
    {L"Africa/Accra",                     L"Greenwich Standard Time"},
    {L"Africa/Algiers",                   L"W. Central Africa Standard Time"},
    {L"Africa/Cairo",                     L"Egypt Standard Time"},
    {L"Africa/Casablanca",                L"Morocco Standard Time"},
    {L"Africa/Johannesburg",              L"South Africa Standard Time"},
    {L"Africa/Lagos",                     L"W. Central Africa Standard Time"},
    {L"Africa/Nairobi",                   L"E. Africa Standard Time"},

    // ------------------------------------------------------------------ UTC
    {L"UTC",                              L"UTC"},
    {L"GMT",                              L"UTC"},
    {L"Etc/UTC",                          L"UTC"},
    {L"Etc/GMT",                          L"UTC"},
    {L"Etc/Greenwich",                    L"UTC"},
};

}  // namespace

bool WindowsZoneForIana(const std::wstring& iana, std::wstring* out) {
    if (!out || iana.empty()) return false;

    // Feeds occasionally quote or pad the identifier, and Outlook writes some
    // zones in mixed case. Neither is worth failing over.
    const wchar_t* name = iana.c_str();
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i) {
        if (_wcsicmp(name, kRows[i].iana) == 0) {
            out->assign(kRows[i].windows);
            return true;
        }
    }
    return false;
}

}  // namespace tzmap
}  // namespace rc
