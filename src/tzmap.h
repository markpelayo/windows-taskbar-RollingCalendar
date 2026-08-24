// tzmap.h — IANA time-zone identifier to Windows zone-key name.
//
// ICS feeds name zones the IANA way ("Europe/London"); Windows names them its
// own way ("GMT Standard Time"), and there is no API that translates between
// the two. The mapping is data, published by CLDR as windowsZones.xml, so the
// only honest implementation is a table. tzmap.cpp carries the practical subset
// rather than the full thousand-odd rows: everything a shared calendar is
// realistically going to name, and nothing else.
//
// A miss is not an error. The caller falls back to the machine's own zone,
// because a strip that is an hour out still tells you when your next meeting
// is, and a strip that failed to load tells you nothing.

#pragma once

#include <string>

namespace rc {
namespace tzmap {

// Case-insensitive lookup. Returns false and leaves `out` untouched when the
// identifier is not in the table; `out` must not be null.
bool WindowsZoneForIana(const std::wstring& iana, std::wstring* out);

}  // namespace tzmap
}  // namespace rc
