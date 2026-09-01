// common.cpp — see common.h for what and why.
//
// Two constraints shape most of what follows. The first is that nothing here
// may pull in the C++ locale machinery: <fstream>, <sstream> and <iostream>
// each construct two complete sets of locale facets at static-initialisation
// time and keep them resident for the life of the process, which for an app
// that spends its life idle in the taskbar is an absurd standing cost. So the
// file I/O is CreateFileW/ReadFile/WriteFile and the formatting is swprintf.
//
// The second is that every function has to be total. Feeds are fetched from
// the internet, settings are hand-edited, and a widget that vanishes because a
// calendar contained a malformed colour is worse than useless. Nothing here
// throws across a module boundary and nothing here asserts on input.

#include "common.h"

#include <windows.h>
#include <shlobj.h>

#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

#include "raii.h"
#include "tzmap.h"

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace rc {

const wchar_t* kAppName = L"RollingCalendar";
const wchar_t* kDisplayName = L"Rolling Calendar";
const wchar_t* kVersion = L"1.1.0";
const wchar_t* kProjectUrl = L"https://github.com/markpelayo/windows-taskbar-RollingCalendar";

namespace {

// The FILETIME epoch is 1601-01-01; the Unix epoch is 1970-01-01. The gap is
// 369 years, of which 89 were leap years.
constexpr int64_t kFileTimeEpochOffset = 11644473600LL;
constexpr int64_t kFileTimeTicksPerSecond = 10000000LL;

// SYSTEMTIME cannot represent anything outside 1601..30827, and the conversion
// APIs fail rather than saturate. Clamping here keeps the failure out of every
// caller: a date this far out is corrupt data, not a case worth reporting.
constexpr Seconds kMinRepresentable = -11644473600LL;  // 1601-01-01T00:00:00Z
constexpr Seconds kMaxRepresentable = 253402300799LL;  // 9999-12-31T23:59:59Z

Seconds ClampRepresentable(Seconds t) {
    if (t < kMinRepresentable) return kMinRepresentable;
    if (t > kMaxRepresentable) return kMaxRepresentable;
    return t;
}

// Howard Hinnant's days_from_civil. Proleptic Gregorian, day 0 = 1970-01-01.
// Written out rather than done with SystemTimeToFileTime round trips because
// day arithmetic must not depend on a time zone, and FILETIME conversions do.
int64_t DaysFromCivil(int y, int m, int d) {
    int64_t year = y - ((m <= 2) ? 1 : 0);
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;                            // [0, 399]
    const int64_t mp = m + ((m > 2) ? -3 : 9);                       // [0, 11]
    const int64_t doy = (153 * mp + 2) / 5 + d - 1;                  // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;       // [0, 146096]
    return era * 146097 + doe - 719468;
}

// The exact inverse, for turning a day number back into a date.
void CivilFromDays(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                            // [0, 146096]
    const int64_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;       // [0, 399]
    int64_t year = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                          // [0, 11]
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;                // [1, 31]
    const int64_t month = mp + ((mp < 10) ? 3 : -9);                 // [1, 12]
    year += (month <= 2) ? 1 : 0;
    if (y) *y = static_cast<int>(year);
    if (m) *m = static_cast<int>(month);
    if (d) *d = static_cast<int>(day);
}

// 1970-01-01 was a Thursday, hence the +4. The double modulus is there because
// day numbers before the epoch are negative and C++ integer modulus is not.
int WeekdayFromDays(int64_t dayNumber) {
    return static_cast<int>(((dayNumber + 4) % 7 + 7) % 7);
}

std::wstring DecodeWith(const char* bytes, size_t count, UINT codePage, DWORD flags) {
    if (!bytes || count == 0) return std::wstring();
    if (count > static_cast<size_t>(INT_MAX)) count = static_cast<size_t>(INT_MAX);
    const int len = static_cast<int>(count);

    const int need = ::MultiByteToWideChar(codePage, flags, bytes, len, nullptr, 0);
    if (need <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(need), L'\0');
    const int got = ::MultiByteToWideChar(codePage, flags, bytes, len, &out[0], need);
    if (got <= 0) return std::wstring();
    out.resize(static_cast<size_t>(got));
    return out;
}

int ClampChannel(double v) {
    if (!(v > 0.0)) return 0;       // also catches NaN, which is the point
    if (v > 255.0) return 255;
    return static_cast<int>(v + 0.5);
}

bool HexNibble(wchar_t c, int* out) {
    if (c >= L'0' && c <= L'9') { *out = c - L'0'; return true; }
    if (c >= L'a' && c <= L'f') { *out = c - L'a' + 10; return true; }
    if (c >= L'A' && c <= L'F') { *out = c - L'A' + 10; return true; }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- time base

Seconds RealNow() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    return FileTimeToUnix(ft);
}

Seconds FileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart) / kFileTimeTicksPerSecond - kFileTimeEpochOffset;
}

FILETIME UnixToFileTime(Seconds t) {
    const int64_t ticks = (ClampRepresentable(t) + kFileTimeEpochOffset) * kFileTimeTicksPerSecond;
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(ticks);
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

Seconds SystemTimeToUnix(const SYSTEMTIME& st) {
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&st, &ft)) {
        // Only reachable with an out-of-range field, which means the SYSTEMTIME
        // came from arithmetic rather than from Windows. Fall back to the day
        // arithmetic, which has no range limit worth worrying about.
        const int64_t days = DaysFromCivil(st.wYear, st.wMonth, st.wDay);
        return days * 86400 + st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
    }
    return FileTimeToUnix(ft);
}

SYSTEMTIME UnixToSystemTime(Seconds t) {
    const FILETIME ft = UnixToFileTime(t);
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&ft, &st)) {
        st.wYear = 1970;
        st.wMonth = 1;
        st.wDay = 1;
    }
    return st;
}

// ---------------------------------------------------------------- time zone

TimeZone::TimeZone() {
    // TIME_ZONE_ID_INVALID still leaves the structure zeroed, which behaves as
    // UTC. That is the correct degraded state: wrong, but monotonic and sane.
    if (::GetDynamicTimeZoneInformation(&tz_) == TIME_ZONE_ID_INVALID) {
        tz_ = DYNAMIC_TIME_ZONE_INFORMATION{};
    }
    isLocal_ = true;
}

bool TimeZone::SetIana(const std::wstring& iana) {
    std::wstring key;
    if (!tzmap::WindowsZoneForIana(iana, &key)) return false;

    // The registry is the only source for a populated DYNAMIC_TIME_ZONE_INFORMATION,
    // and the enumeration is the only supported way to read it. The iteration
    // cap guards against a driver or policy returning an error code other than
    // ERROR_NO_MORE_ITEMS forever; there are around 140 zones on a current build.
    DYNAMIC_TIME_ZONE_INFORMATION info{};
    for (DWORD i = 0; i < 1024u; ++i) {
        const DWORD result = ::EnumDynamicTimeZoneInformation(i, &info);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;
        if (_wcsicmp(info.TimeZoneKeyName, key.c_str()) == 0) {
            tz_ = info;
            iana_ = iana;
            isLocal_ = false;
            return true;
        }
    }
    return false;
}

TimeZone::Parts TimeZone::Break(Seconds t) const {
    const SYSTEMTIME utc = UnixToSystemTime(t);
    SYSTEMTIME local{};
    if (!::SystemTimeToTzSpecificLocalTimeEx(&tz_, &utc, &local)) local = utc;

    Parts p;
    p.year = local.wYear;
    p.month = local.wMonth;
    p.day = local.wDay;
    p.hour = local.wHour;
    p.minute = local.wMinute;
    p.second = local.wSecond;
    // wDayOfWeek is documented as set on output but is derived cheaply enough
    // that trusting the arithmetic costs nothing and removes a dependency.
    p.weekday = WeekdayFromDays(DaysFromCivil(p.year, p.month, p.day));
    return p;
}

Seconds TimeZone::Make(int year, int month, int day, int hour, int minute, int second) const {
    // Normalise first. Callers do things like "the 32nd of January" when they
    // step a day forward, and TzSpecificLocalTimeToSystemTimeEx simply fails on
    // an out-of-range field rather than carrying it.
    int64_t carryMonths = static_cast<int64_t>(month) - 1;
    int64_t y = static_cast<int64_t>(year) + carryMonths / 12;
    int64_t m = carryMonths % 12;
    if (m < 0) { m += 12; y -= 1; }

    int64_t dayNumber = DaysFromCivil(static_cast<int>(y), static_cast<int>(m) + 1, 1) +
                        (static_cast<int64_t>(day) - 1);
    int64_t secondsOfDay = static_cast<int64_t>(hour) * 3600 +
                           static_cast<int64_t>(minute) * 60 +
                           static_cast<int64_t>(second);
    dayNumber += secondsOfDay / 86400;
    secondsOfDay %= 86400;
    if (secondsOfDay < 0) { secondsOfDay += 86400; dayNumber -= 1; }

    int ny = 1970, nm = 1, nd = 1;
    CivilFromDays(dayNumber, &ny, &nm, &nd);
    if (ny < 1601 || ny > 9999) return ClampRepresentable(dayNumber * 86400 + secondsOfDay);

    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(ny);
    local.wMonth = static_cast<WORD>(nm);
    local.wDay = static_cast<WORD>(nd);
    local.wHour = static_cast<WORD>(secondsOfDay / 3600);
    local.wMinute = static_cast<WORD>((secondsOfDay % 3600) / 60);
    local.wSecond = static_cast<WORD>(secondsOfDay % 60);

    SYSTEMTIME utc{};
    if (!::TzSpecificLocalTimeToSystemTimeEx(&tz_, &local, &utc)) {
        // A local time that does not exist (the hour skipped in spring) lands
        // here on some builds. Treating it as UTC is at most an offset out, and
        // it keeps the function total.
        return SystemTimeToUnix(local);
    }
    return SystemTimeToUnix(utc);
}

Seconds TimeZone::StartOfDay(Seconds t) const {
    const Parts p = Break(t);
    return Make(p.year, p.month, p.day, 0, 0, 0);
}

int TimeZone::DayDifference(Seconds a, Seconds b) const {
    const Parts pa = Break(a);
    const Parts pb = Break(b);
    const int64_t diff = DaysFromCivil(pb.year, pb.month, pb.day) -
                         DaysFromCivil(pa.year, pa.month, pa.day);
    return static_cast<int>(diff);
}

int TimeZone::IsoWeek(Seconds t) const {
    const Parts p = Break(t);
    const int64_t dayNumber = DaysFromCivil(p.year, p.month, p.day);

    // ISO 8601 anchors a week to the Thursday it contains: whichever calendar
    // year that Thursday falls in owns the week. That is the whole rule, and it
    // is why a date in early January can legitimately be week 52 of last year.
    const int isoWeekday = (p.weekday == 0) ? 7 : p.weekday;  // 1 = Monday
    const int64_t thursday = dayNumber - isoWeekday + 4;

    int thursdayYear = 1970, thursdayMonth = 1, thursdayDay = 1;
    CivilFromDays(thursday, &thursdayYear, &thursdayMonth, &thursdayDay);
    const int64_t jan1 = DaysFromCivil(thursdayYear, 1, 1);
    const int64_t week = (thursday - jan1) / 7 + 1;

    if (week < 1) return 1;
    if (week > 53) return 53;
    return static_cast<int>(week);
}

// ------------------------------------------------------------ simulated clock

namespace Clock {
namespace {
// Plain double rather than an atomic: it is written only by the settings UI on
// the main thread and read by the paint path on the same thread. A torn read on
// a background fetch thread would shift a block by a frame at worst.
double g_offset = 0.0;
constexpr double kMaxOffset = 3155760000.0;  // ~100 years
}  // namespace

void SetOffset(double seconds) {
    if (!std::isfinite(seconds)) return;  // a NaN offset would poison every date
    if (seconds > kMaxOffset) seconds = kMaxOffset;
    if (seconds < -kMaxOffset) seconds = -kMaxOffset;
    g_offset = seconds;
}

double Offset() { return g_offset; }

bool IsSimulating() { return g_offset != 0.0; }

Seconds Now() { return RealNow() + static_cast<Seconds>(g_offset); }

}  // namespace Clock

// ------------------------------------------------------------------ strings

std::wstring Widen(const std::string& utf8) {
    return DecodeWith(utf8.data(), utf8.size(), CP_UTF8, 0);
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    size_t count = s.size();
    if (count > static_cast<size_t>(INT_MAX)) count = static_cast<size_t>(INT_MAX);
    const int len = static_cast<int>(count);

    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();

    std::string out(static_cast<size_t>(need), '\0');
    const int got =
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), len, &out[0], need, nullptr, nullptr);
    if (got <= 0) return std::string();
    out.resize(static_cast<size_t>(got));
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && iswspace(s[begin])) ++begin;
    while (end > begin && iswspace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

std::wstring Lower(const std::wstring& s) {
    std::wstring out(s);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<wchar_t>(towlower(out[i]));
    }
    return out;
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return wcsncmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    return _wcsnicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str(), suffix.size()) == 0;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    if (s.empty()) return out;  // one empty field is never what a caller wants

    size_t start = 0;
    for (;;) {
        const size_t hit = s.find(sep, start);
        if (hit == std::wstring::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, hit - start));
        start = hit + 1;
    }
    return out;
}

std::wstring Format(const wchar_t* fmt, ...) {
    if (!fmt) return std::wstring();

    va_list args;
    va_start(args, fmt);
    va_list measure;
    va_copy(measure, args);

    // Measure rather than guess: the keyword and calendar names in this app are
    // user-supplied and there is no honest upper bound on a fixed buffer.
    const int needed = _vscwprintf(fmt, measure);
    va_end(measure);
    if (needed < 0) {
        va_end(args);
        return std::wstring();
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1, L'\0');
    const int written = _vsnwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, fmt, args);
    va_end(args);

    if (written < 0) return std::wstring(buffer.data());
    return std::wstring(buffer.data(), static_cast<size_t>(written));
}

std::wstring Normalize(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());

    bool pendingSpace = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (iswalnum(c)) {
            if (pendingSpace && !out.empty()) out.push_back(L' ');
            pendingSpace = false;
            out.push_back(static_cast<wchar_t>(towlower(c)));
        } else {
            // Collapsing is deferred rather than done eagerly, which is what
            // trims the leading and trailing runs for free.
            pendingSpace = true;
        }
    }
    return out;
}

bool ContainsWord(const std::wstring& normalizedHaystack, const std::wstring& normalizedNeedle) {
    if (normalizedNeedle.empty()) return false;
    if (normalizedHaystack.empty()) return false;

    // Padding both sides turns a substring search into a word-boundary search
    // without a regex engine: "art" no longer matches "start".
    const std::wstring haystack = L" " + normalizedHaystack + L" ";
    const std::wstring needle = L" " + normalizedNeedle + L" ";
    return haystack.find(needle) != std::wstring::npos;
}

std::wstring FormatDuration(double seconds) {
    if (!std::isfinite(seconds)) return L"0s";

    // Round up, so the countdown only reads zero once the block has genuinely
    // ended rather than for the last half-second of it.
    double value = std::ceil(seconds);

    // Clamp while still a double. Casting an out-of-range double to an integer
    // is undefined behaviour, and on x64 it yields INT64_MIN rather than a
    // saturated value, which would print a ten-digit negative hour count.
    if (!(value > 0.0)) value = 0.0;
    if (value > 315360000.0) value = 315360000.0;  // ten years

    const int64_t total = static_cast<int64_t>(value);
    const int64_t hours = total / 3600;
    const int64_t minutes = (total % 3600) / 60;
    const int64_t secs = total % 60;

    if (hours > 0) {
        return minutes > 0 ? Format(L"%lldh%02lld", hours, minutes) : Format(L"%lldh", hours);
    }
    if (minutes > 0) return Format(L"%lldm", minutes);
    return Format(L"%llds", secs);
}

// ------------------------------------------------------------------- colours

bool ParseHexColor(const std::wstring& s, COLORREF* out) {
    if (!out) return false;

    std::wstring hex = Trim(s);
    if (!hex.empty() && hex[0] == L'#') hex.erase(0, 1);

    int v[6] = {0, 0, 0, 0, 0, 0};
    if (hex.size() == 6) {
        for (size_t i = 0; i < 6; ++i) {
            if (!HexNibble(hex[i], &v[i])) return false;
        }
    } else if (hex.size() == 3) {
        // "#abc" means "#aabbcc": each nibble is doubled, not zero-extended,
        // so "#fff" is white rather than a dark grey.
        for (size_t i = 0; i < 3; ++i) {
            int n = 0;
            if (!HexNibble(hex[i], &n)) return false;
            v[i * 2] = n;
            v[i * 2 + 1] = n;
        }
    } else {
        return false;
    }

    const int r = v[0] * 16 + v[1];
    const int g = v[2] * 16 + v[3];
    const int b = v[4] * 16 + v[5];
    *out = RGB(r, g, b);
    return true;
}

std::wstring ColorToHex(COLORREF c) {
    return Format(L"#%02x%02x%02x",
                  static_cast<int>(GetRValue(c)),
                  static_cast<int>(GetGValue(c)),
                  static_cast<int>(GetBValue(c)));
}

COLORREF Blend(COLORREF a, COLORREF b, double t) {
    if (!std::isfinite(t)) return a;
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;

    const double ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    const double br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    return RGB(ClampChannel(ar + (br - ar) * t),
               ClampChannel(ag + (bg - ag) * t),
               ClampChannel(ab + (bb - ab) * t));
}

COLORREF Highlight(COLORREF c, double level) {
    // NSColor.highlight(withLevel:) is a straight interpolation toward white,
    // which is what the macOS original relies on for the hover and "now" tints.
    return Blend(c, RGB(255, 255, 255), level);
}

// ------------------------------------------------------------------- system

bool IsDarkMode() {
    // Memoised for two seconds. The paint path asks this question several times
    // per redraw, and a registry read per question is a syscall per question;
    // meanwhile the answer changes at most when the user flips a switch in
    // Settings. Two seconds is short enough that even a caller which ignores
    // WM_SETTINGCHANGE catches up without the user noticing. App refreshes the
    // rest of its theme state on WM_SETTINGCHANGE regardless, so this cache is
    // a floor on staleness rather than the mechanism.
    static ULONGLONG lastCheck = 0;
    static bool cached = false;
    static bool primed = false;

    const ULONGLONG now = ::GetTickCount64();
    if (primed && now - lastCheck < 2000ull) return cached;

    DWORD value = 1;  // absent key means light, which is the pre-1809 default
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = ::RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, &type, &value, &size);

    cached = (status == ERROR_SUCCESS) && (value == 0);
    lastCheck = now;
    primed = true;
    return cached;
}

int DpiForWindow(HWND hwnd) {
    // GetDpiForWindow arrived in 1607. Resolving it dynamically keeps the app
    // loadable on earlier builds, where the system-wide DPI is the only figure
    // available anyway because per-monitor awareness did not exist either.
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getDpi = nullptr;
    static bool resolved = false;

    if (!resolved) {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32) {
            getDpi = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        }
        resolved = true;
    }

    if (getDpi && hwnd) {
        const UINT dpi = getDpi(hwnd);
        if (dpi > 0) return static_cast<int>(dpi);
    }

    const HDC screen = ::GetDC(nullptr);
    int dpi = 96;
    if (screen) {
        const int reported = ::GetDeviceCaps(screen, LOGPIXELSX);
        if (reported > 0) dpi = reported;
        ::ReleaseDC(nullptr, screen);
    }
    return dpi;
}

std::wstring AppDataDir() {
    wchar_t path[MAX_PATH] = {0};
    // SHGetFolderPathW rather than SHGetKnownFolderPath: it needs no CoTaskMemFree
    // and no uuid.lib, and the roaming AppData folder is one of the CSIDL values
    // the shim has always handled correctly.
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr,
                                  SHGFP_TYPE_CURRENT, path))) {
        return std::wstring();
    }

    std::wstring dir(path);
    if (dir.empty()) return dir;
    if (dir[dir.size() - 1] != L'\\') dir.push_back(L'\\');
    dir += kAppName;

    // Created on demand rather than at install time, because the app is also
    // expected to run from a folder someone unzipped.
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        if (::GetLastError() != ERROR_ALREADY_EXISTS) return std::wstring();
    }
    return dir;
}

std::wstring ExecutablePath() {
    // GetModuleFileNameW truncates rather than telling you how much it needed,
    // so the only reliable test is whether the buffer came back exactly full.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return std::wstring();
        if (written < buffer.size()) return std::wstring(buffer.data(), written);
        if (buffer.size() >= 32768u) return std::wstring(buffer.data(), buffer.size());
        buffer.resize(buffer.size() * 2);
    }
}

bool ReadFileText(const std::wstring& path, std::wstring* out) {
    if (!out || path.empty()) return false;

    UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) return false;
    if (size.QuadPart < 0) return false;
    // A settings file or an ICS feed larger than this is not a settings file or
    // an ICS feed; refusing beats allocating 4 GB on a mistyped path.
    if (size.QuadPart > (64LL << 20)) return false;

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t filled = 0;
    while (filled < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (bytes.size() - filled) > 0x10000000u ? 0x10000000u : (bytes.size() - filled));
        DWORD read = 0;
        if (!::ReadFile(file.get(), &bytes[filled], chunk, &read, nullptr)) return false;
        if (read == 0) break;  // shorter than advertised; take what arrived
        filled += read;
    }
    bytes.resize(filled);

    *out = DecodeText(bytes);
    return true;
}

bool WriteFileText(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) return false;

    // UTF-8 with no BOM. A BOM would be legal but it breaks every naive line
    // parser that opens the settings file in a text editor and expects the
    // first key to start at byte zero.
    const std::string bytes = Narrow(text);

    UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;

    size_t written = 0;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (bytes.size() - written) > 0x10000000u ? 0x10000000u : (bytes.size() - written));
        DWORD done = 0;
        if (!::WriteFile(file.get(), bytes.data() + written, chunk, &done, nullptr)) return false;
        if (done == 0) return false;
        written += done;
    }
    return true;
}

std::wstring DecodeText(const std::string& bytes) {
    if (bytes.empty()) return std::wstring();

    const unsigned char* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t size = bytes.size();

    if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        return DecodeWith(bytes.data() + 3, size - 3, CP_UTF8, 0);
    }

    // UTF-16 turns up when someone has saved the file from PowerShell, whose
    // redirection operators still default to it.
    if (size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        std::wstring out((size - 2) / 2, L'\0');
        ::memcpy(&out[0], bytes.data() + 2, out.size() * sizeof(wchar_t));
        return out;
    }
    if (size >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        std::wstring out((size - 2) / 2, L'\0');
        for (size_t i = 0; i < out.size(); ++i) {
            const unsigned hi = raw[2 + i * 2];
            const unsigned lo = raw[3 + i * 2];
            out[i] = static_cast<wchar_t>((hi << 8) | lo);
        }
        return out;
    }

    // MB_ERR_INVALID_CHARS turns a lenient decode into a validity test: if the
    // bytes are not well-formed UTF-8 the call fails outright, and anything
    // that fails is far more likely to be Latin-1 than to be broken UTF-8.
    std::wstring utf8 = DecodeWith(bytes.data(), size, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!utf8.empty()) return utf8;

    return DecodeWith(bytes.data(), size, 1252, 0);
}

}  // namespace rc
