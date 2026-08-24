// common.h — shared primitives: time, time zones, strings, colours.
//
// The macOS original leans on Foundation for all of this. Here it is spelled
// out, because the alternative is dragging in ICU or the C++ iostreams/locale
// machinery, and both cost more at startup than the whole rest of the app.
//
// Time is held as int64_t seconds since the Unix epoch throughout. It is the
// only representation that survives a round trip through a time zone, an INI
// file and an ICS feed without an off-by-a-day argument.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rc {

// ---------------------------------------------------------------- time base

using Seconds = int64_t;  // Unix epoch seconds, UTC

// Real wall clock, independent of the simulated offset.
Seconds RealNow();

// FILETIME / SYSTEMTIME bridges. SystemTime values here are always UTC.
Seconds FileTimeToUnix(const FILETIME& ft);
FILETIME UnixToFileTime(Seconds t);
Seconds SystemTimeToUnix(const SYSTEMTIME& st);
SYSTEMTIME UnixToSystemTime(Seconds t);

// ---------------------------------------------------------------- time zone
//
// A resolved time zone. Constructed from an IANA identifier ("Europe/London")
// when a feed carries `ctz=`, otherwise from the machine's own setting.
//
// IANA -> Windows zone-key mapping is a lookup against a table lifted from the
// CLDR windowsZones supplemental data (the common subset; see tzmap.cpp). An
// unrecognised identifier falls back to the local zone rather than failing, on
// the grounds that a slightly wrong clock beats a blank strip.
class TimeZone {
public:
    TimeZone();  // machine local zone

    // Returns false and leaves the zone as local if `iana` is not recognised.
    bool SetIana(const std::wstring& iana);

    const std::wstring& Iana() const { return iana_; }
    bool IsLocal() const { return isLocal_; }

    // Broken-down local time in this zone.
    struct Parts {
        int year = 1970, month = 1, day = 1;
        int hour = 0, minute = 0, second = 0;
        int weekday = 4;  // 0 = Sunday
    };

    Parts Break(Seconds t) const;

    // Wall-clock -> epoch. Ambiguous or non-existent local times (DST edges)
    // resolve the way Windows resolves them; no exception, no failure.
    Seconds Make(int year, int month, int day, int hour, int minute, int second) const;

    // Midnight opening the day containing `t`.
    Seconds StartOfDay(Seconds t) const;

    // Days between the two dates, ignoring time of day. Positive when b > a.
    int DayDifference(Seconds a, Seconds b) const;

    // ISO 8601 week number (1-53). GetDateFormat cannot produce this.
    int IsoWeek(Seconds t) const;

private:
    std::wstring iana_;
    bool isLocal_ = true;
    DYNAMIC_TIME_ZONE_INFORMATION tz_{};
};

// ------------------------------------------------------------- simulated clock
//
// Debug Time is an *offset*, not a freeze: the simulated clock keeps advancing,
// so blocks still slide and countdowns still tick.
namespace Clock {
void SetOffset(double seconds);   // clamped to +/- 3155760000 (~100 years)
double Offset();
bool IsSimulating();
Seconds Now();                    // RealNow() + Offset()
}  // namespace Clock

// ------------------------------------------------------------------ strings

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& s);
std::wstring Trim(const std::wstring& s);
std::wstring Lower(const std::wstring& s);
bool StartsWith(const std::wstring& s, const std::wstring& prefix);
bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);
std::wstring Format(const wchar_t* fmt, ...);

// Reduces text to lowercase words: every non-alphanumeric becomes a space and
// runs of spaces collapse. "Focus Work | Learn (2H)" -> "focus work learn 2h".
std::wstring Normalize(const std::wstring& text);

// Whole-word containment over already-normalized strings.
bool ContainsWord(const std::wstring& normalizedHaystack, const std::wstring& normalizedNeedle);

// "1h05", "1h", "12m", "45s". Rounds up, so it only reads zero once the block
// has genuinely ended. Clamped before the integer conversion.
std::wstring FormatDuration(double seconds);

// ------------------------------------------------------------------- colours

// Accepts "#rrggbb", "rrggbb", "#rgb". Returns false on anything else.
bool ParseHexColor(const std::wstring& s, COLORREF* out);
std::wstring ColorToHex(COLORREF c);

// Blend toward white by `level` (NSColor.highlight(withLevel:)).
COLORREF Highlight(COLORREF c, double level);
COLORREF Blend(COLORREF a, COLORREF b, double t);

// ------------------------------------------------------------------- system

bool IsDarkMode();                 // AppsUseLightTheme, inverted
int  DpiForWindow(HWND hwnd);
std::wstring AppDataDir();         // %APPDATA%\RollingCalendar, created on demand
std::wstring ExecutablePath();

// UTF-8 / UTF-16 / Latin-1 file read. Win32 calls only: <fstream> drags in two
// full sets of locale facets that construct at startup and stay resident.
bool ReadFileText(const std::wstring& path, std::wstring* out);
bool WriteFileText(const std::wstring& path, const std::wstring& text);

// Decodes a byte buffer that is expected to be UTF-8, falling back to Latin-1
// when it does not validate (some hand-edited CSVs are Latin-1).
std::wstring DecodeText(const std::string& bytes);

extern const wchar_t* kAppName;      // L"RollingCalendar"
extern const wchar_t* kDisplayName;  // L"Rolling Calendar"
extern const wchar_t* kVersion;      // L"1.0.0"
extern const wchar_t* kProjectUrl;

}  // namespace rc
