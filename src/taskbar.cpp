// taskbar.cpp — see taskbar.h for the argument about why this exists at all.
//
// Everything here is defensive. The taskbar is somebody else's window and its
// internal structure is not contractual, so each step is allowed to fail and
// each failure has a defined, visible fallback rather than an assertion. The
// worst outcome the app permits itself is a floating strip parked over the
// taskbar; a strip that is simply absent would look like a crash.

#include "taskbar.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cwchar>

#include "common.h"
#include "timeline.h"

namespace rc {
namespace {

int ScaleForDpi(int px, int dpi) { return ::MulDiv(px, dpi, 96); }

int RectWidth(const RECT& r) { return r.right - r.left; }
int RectHeight(const RECT& r) { return r.bottom - r.top; }

bool IsHorizontal(TaskbarEdge edge) {
    return edge == TaskbarEdge::Bottom || edge == TaskbarEdge::Top;
}

// State for the notification-area hunt below. `exact` records that the real
// TrayNotifyWnd turned up, in which case nothing else is considered.
struct NotifyScan {
    RECT tray{};
    bool horizontal = true;
    HWND best = nullptr;
    RECT bestRect{};
    bool exact = false;
};

BOOL CALLBACK ScanForNotifyArea(HWND child, LPARAM param) {
    NotifyScan* scan = reinterpret_cast<NotifyScan*>(param);

    RECT r{};
    if (!::GetWindowRect(child, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    if (!::IsWindowVisible(child)) return TRUE;

    wchar_t cls[64] = {};
    ::GetClassNameW(child, cls, 64);

    if (::wcscmp(cls, L"TrayNotifyWnd") == 0) {
        scan->best = child;
        scan->bestRect = r;
        scan->exact = true;
        return FALSE;  // nothing better exists; stop walking
    }

    // Windows 11 moved the clock and the status icons into XAML islands, so the
    // classic child is sometimes missing. Falling back on geometry -- the
    // trailing cluster of the taskbar, occupying well under half its length --
    // finds the same region without naming any undocumented class.
    const int trayLength = scan->horizontal ? RectWidth(scan->tray) : RectHeight(scan->tray);
    const int length = scan->horizontal ? RectWidth(r) : RectHeight(r);
    if (trayLength <= 0 || length < 8 || length > trayLength / 2) return TRUE;

    const int start = scan->horizontal ? r.left : r.top;
    const int bestStart = scan->horizontal ? scan->bestRect.left : scan->bestRect.top;
    if (!scan->best || start > bestStart) {
        scan->best = child;
        scan->bestRect = r;
    }
    return TRUE;
}

// SystemUsesLightTheme, missing on builds that predate the setting. Those
// defaulted to a dark taskbar, so absence reads as dark.
//
// Memoised, in the same shape and for the same reason as IsDarkMode() in
// common.cpp: the paint path asks for the taskbar's text colour four times a
// second -- twice to decide whether the bar is dark, once to colour the labels,
// and once more because the strip is measured and painted separately -- and the
// answer changes only when the user flips a switch in Settings. A registry read
// per question is a syscall per question, for as many weeks as the machine is
// left on.
//
// The age test is not the mechanism, it is the floor. App calls
// ForgetTaskbarTheme() on WM_SETTINGCHANGE, so a theme change is picked up on
// the very next repaint; the two seconds only cover a machine that never sends
// the notification. Touched from the UI thread only, so it is unsynchronised.
ULONGLONG g_themeChecked = 0;
bool g_themePrimed = false;
bool g_themeIsLight = false;

bool TaskbarUsesLightTheme() {
    const ULONGLONG now = ::GetTickCount64();
    if (g_themePrimed && now - g_themeChecked < 2000ull) return g_themeIsLight;

    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS st = ::RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);

    g_themeIsLight = (st == ERROR_SUCCESS) && (value != 0);
    g_themeChecked = now;
    g_themePrimed = true;
    return g_themeIsLight;
}

}  // namespace

namespace {

// Fills in everything about one taskbar window. Split out of QueryTaskbar so
// that a secondary bar can be described the same way as the primary one --
// there is nothing special about the primary except that it is easy to find.
TaskbarInfo BuildTaskbarInfo(HWND tray) {
    TaskbarInfo info;

    if (!tray) return info;
    if (!::GetWindowRect(tray, &info.bounds)) return info;
    if (RectWidth(info.bounds) <= 0 || RectHeight(info.bounds) <= 0) return info;

    info.window = tray;
    info.dpi = DpiForWindow(tray);

    // The edge is inferred from geometry rather than asked for, because
    // SHAppBarMessage(ABM_GETTASKBARPOS) reports the primary taskbar only and
    // the shell has never had an API for "which side is this particular bar on".
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    const HMONITOR mon = ::MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY);
    RECT screen{};
    if (mon && ::GetMonitorInfoW(mon, reinterpret_cast<MONITORINFO*>(&mi))) {
        screen = mi.rcMonitor;
        // The adapter's device name, \\.\DISPLAY2 and so on. Stored rather
        // than a monitor index because indices are reassigned when a display is
        // unplugged, and a preference that silently means a different screen
        // after a dock is worse than one that fails to apply.
        info.monitorDevice = mi.szDevice;
        info.isPrimaryMonitor = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    } else {
        screen.left = 0;
        screen.top = 0;
        screen.right = ::GetSystemMetrics(SM_CXSCREEN);
        screen.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    }

    if (RectWidth(info.bounds) >= RectHeight(info.bounds)) {
        const int midpoint = screen.top + RectHeight(screen) / 2;
        info.edge = (info.bounds.top < midpoint) ? TaskbarEdge::Top : TaskbarEdge::Bottom;
    } else {
        const int midpoint = screen.left + RectWidth(screen) / 2;
        info.edge = (info.bounds.left < midpoint) ? TaskbarEdge::Left : TaskbarEdge::Right;
    }

    NotifyScan scan;
    scan.tray = info.bounds;
    scan.horizontal = IsHorizontal(info.edge);

    // Ask for the documented child first; only if that comes back empty does
    // the full descendant walk run, which is the Windows 11 case.
    const HWND direct = ::FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (direct && ::GetWindowRect(direct, &scan.bestRect)) {
        scan.best = direct;
        scan.exact = true;
    } else {
        ::EnumChildWindows(tray, ScanForNotifyArea, reinterpret_cast<LPARAM>(&scan));
    }

    if (scan.best) {
        info.notifyBounds = scan.bestRect;  // screen coordinates, as GetWindowRect gives them
        info.hasNotifyArea = true;
    }

    // Windows 11 hosts the whole taskbar UI in a composition island. Its
    // presence decides whether an ordinary GDI child can be seen at all, so it
    // is worth knowing about before the first paint rather than after a user
    // reports a blank taskbar.
    info.compositedShell =
        ::FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge",
                        nullptr) != nullptr;

    info.valid = true;
    return info;
}

BOOL CALLBACK CollectTaskbars(HWND hwnd, LPARAM param) {
    wchar_t cls[64] = {0};
    ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));

    // The primary bar and the per-monitor ones are different window classes.
    // The secondary class only exists while "Show my taskbar on all displays"
    // is on, so a single-entry result does not necessarily mean a single
    // monitor.
    const bool isTaskbar = (_wcsicmp(cls, L"Shell_TrayWnd") == 0) ||
                           (_wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0);
    if (!isTaskbar) return TRUE;

    auto* out = reinterpret_cast<std::vector<TaskbarInfo>*>(param);
    TaskbarInfo info = BuildTaskbarInfo(hwnd);
    if (info.valid) out->push_back(std::move(info));
    return TRUE;
}

}  // namespace

std::vector<TaskbarInfo> EnumerateTaskbars() {
    std::vector<TaskbarInfo> bars;
    ::EnumWindows(CollectTaskbars, reinterpret_cast<LPARAM>(&bars));

    // Primary first, then left to right. The order is what the menu shows, and
    // a list that reorders itself between openings would be unusable.
    std::stable_sort(bars.begin(), bars.end(), [](const TaskbarInfo& a, const TaskbarInfo& b) {
        if (a.isPrimaryMonitor != b.isPrimaryMonitor) return a.isPrimaryMonitor;
        return a.bounds.left < b.bounds.left;
    });
    return bars;
}

std::wstring TaskbarInfo::MonitorLabel() const {
    const int w = RectWidth(bounds);
    wchar_t buf[160];

    // The device name is not something to show a person, so it is reduced to
    // its trailing digits: \\.\DISPLAY2 becomes "Display 2".
    std::wstring number;
    for (auto it = monitorDevice.rbegin(); it != monitorDevice.rend(); ++it) {
        if (*it >= L'0' && *it <= L'9') {
            number.insert(number.begin(), *it);
        } else if (!number.empty()) {
            break;
        }
    }
    if (number.empty()) number = L"?";

    ::swprintf_s(buf, L"Display %s \u2014 %d px wide%s", number.c_str(), w,
                 isPrimaryMonitor ? L" (primary)" : L"");
    return buf;
}

TaskbarInfo QueryTaskbar(const std::wstring& preferredDevice) {
    const std::vector<TaskbarInfo> bars = EnumerateTaskbars();

    if (bars.empty()) {
        // EnumWindows found nothing, which should not happen while Explorer is
        // running. Ask for the primary bar by name anyway rather than reporting
        // no taskbar at all.
        return BuildTaskbarInfo(::FindWindowW(L"Shell_TrayWnd", nullptr));
    }

    if (!preferredDevice.empty()) {
        for (const TaskbarInfo& bar : bars) {
            if (_wcsicmp(bar.monitorDevice.c_str(), preferredDevice.c_str()) == 0) return bar;
        }
    }

    for (const TaskbarInfo& bar : bars) {
        if (bar.isPrimaryMonitor) return bar;
    }
    return bars.front();
}

bool MakeCompositedChild(HWND child) {
    if (!child) return false;

    const LONG_PTR ex = ::GetWindowLongPtrW(child, GWL_EXSTYLE);
    ::SetLastError(0);
    ::SetWindowLongPtrW(child, GWL_EXSTYLE, ex | static_cast<LONG_PTR>(WS_EX_LAYERED));

    // Two things at once. The alpha is not an effect -- it is what asks DWM to
    // redirect this window into a composition visual of its own, which is what
    // puts it above the shell's island. The colour key is what makes the strip
    // look like part of the taskbar instead of a slab laid over it: the paint
    // code fills its background with exactly this colour, and it vanishes.
    //
    // Guessing the taskbar's colour instead was never going to work. With
    // transparency effects on, the bar is acrylic over the user's wallpaper,
    // so there is no colour to guess.
    if (!::SetLayeredWindowAttributes(child, timeline::ChromaKey(), 255,
                                      LWA_COLORKEY | LWA_ALPHA)) {
        ::SetWindowLongPtrW(child, GWL_EXSTYLE, ex);
        return false;
    }

    return true;
}

int UsableThickness(const TaskbarInfo& info) {
    if (!info.valid) return 0;

    const int thickness = IsHorizontal(info.edge) ? RectHeight(info.bounds) : RectWidth(info.bounds);
    const int margin = ScaleForDpi(4, info.dpi);
    const int usable = thickness - 2 * margin;

    // A taskbar this thin means something has gone wrong with the measurement
    // rather than that the user wants a two-pixel widget, so floor it and let
    // the strip overhang slightly instead of collapsing.
    const int floorPx = ScaleForDpi(12, info.dpi);
    return std::max(usable, floorPx);
}

HostMode EmbedInTaskbar(HWND child, const TaskbarInfo& info) {
    if (!child || !info.valid || !info.window) return HostMode::Floating;

    // This is the one undocumented thing the app does: re-parenting an ordinary
    // window into Shell_TrayWnd so the shell moves, hides and auto-hides it for
    // us. It is built entirely out of documented calls, but the shell never
    // promised to tolerate a stranger among its children, so the result is
    // verified rather than assumed.
    ::SetParent(child, info.window);

    // SetParent returns the previous parent, and null is both "it failed" and
    // "it had no parent". So the result is confirmed by asking, not by the
    // return value.
    if (::GetParent(child) != info.window) return HostMode::Floating;

    // A child window cannot be topmost, and leaving the bit set confuses the
    // z-order bookkeeping that SetWindowPos does on the next move.
    const LONG_PTR ex = ::GetWindowLongPtrW(child, GWL_EXSTYLE);
    ::SetWindowLongPtrW(child, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
    return HostMode::Embedded;
}

void DetachFromTaskbar(HWND child) {
    if (!child) return;

    ::SetParent(child, nullptr);

    const LONG_PTR ex = ::GetWindowLongPtrW(child, GWL_EXSTYLE);
    ::SetWindowLongPtrW(child, GWL_EXSTYLE, ex | static_cast<LONG_PTR>(WS_EX_TOPMOST));
    ::SetWindowPos(child, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void PositionWidget(HWND child,
                    const TaskbarInfo& info,
                    HostMode mode,
                    int offsetAlong,
                    int width,
                    int thickness) {
    if (!child || !info.valid) return;

    const bool horizontal = IsHorizontal(info.edge);
    const int barThickness = horizontal ? RectHeight(info.bounds) : RectWidth(info.bounds);
    const int centred = std::max(0, (barThickness - thickness) / 2);

    // `width` is always the length along the bar and `thickness` always across
    // it, so a left- or right-docked taskbar simply swaps which is which.
    int x = 0;
    int y = 0;
    int cx = 0;
    int cy = 0;

    if (horizontal) {
        x = offsetAlong;
        y = centred;
        cx = width;
        cy = thickness;
    } else {
        x = centred;
        y = offsetAlong;
        cx = thickness;
        cy = width;
    }

    if (mode == HostMode::Floating) {
        x += info.bounds.left;
        y += info.bounds.top;
    }

    // In embedded mode the strip is raised to the top of the sibling order on
    // every move, not left where SetParent happened to put it.
    //
    // This was the original bug: with SWP_NOZORDER the strip is inserted at the
    // bottom of Shell_TrayWnd's children and the taskbar's own content draws
    // over it. On a machine with taskbar transparency the result is peculiar
    // rather than absent -- you can see the strip's colours bleeding faintly
    // through the bar, which reads as a rendering fault rather than a z-order
    // one.
    ::SetWindowPos(child, (mode == HostMode::Floating) ? HWND_TOPMOST : HWND_TOP, x, y, cx, cy,
                   SWP_NOACTIVATE);
}

void RaiseWithinTaskbar(HWND child) {
    if (!child) return;

    // The shell reorders its children when it relayouts, and it has no reason
    // to keep a stranger on top. Re-asserting costs one call against a window
    // that is already first, which is why the periodic poll can afford to do it
    // unconditionally rather than trying to detect when it is needed.
    ::SetWindowPos(child, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

namespace {

// Classes the shell puts in its own taskbar. Anything else embedded there was
// put there by another application doing what this one does.
bool IsShellOwnClass(const wchar_t* cls) {
    static const wchar_t* const kShellClasses[] = {
        L"TrayNotifyWnd",       L"MSTaskSwWClass",      L"MSTaskListWClass",
        L"ReBarWindow32",       L"ToolbarWindow32",     L"TrayClockWClass",
        L"TrayShowDesktopButtonWClass", L"TrayButton",  L"Start",
        L"TrayDummySearchControl", L"TrayInputIndicatorWClass",
        L"NotifyIconOverflowWindow", L"SystemTray_Main",
        L"Windows.UI.Input.InputSite.WindowClass",
        L"Windows.UI.Composition.DesktopWindowContentBridge",
    };
    for (const wchar_t* known : kShellClasses) {
        if (_wcsicmp(cls, known) == 0) return true;
    }
    return false;
}

struct Span {
    int start = 0;
    int end = 0;
};

struct ForeignScan {
    Span spans[16]{};
    int count = 0;
    bool horizontal = true;
};

BOOL CALLBACK ForeignChildProc(HWND child, LPARAM param) {
    ForeignScan* scan = reinterpret_cast<ForeignScan*>(param);

    if (!::IsWindowVisible(child)) return TRUE;

    // Our own strip is a child here too, and parking to the left of ourselves
    // would walk the widget off the end of the bar one tick at a time.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(child, &pid);
    if (pid == ::GetCurrentProcessId()) return TRUE;

    wchar_t cls[128] = {0};
    ::GetClassNameW(child, cls, ARRAYSIZE(cls));
    if (IsShellOwnClass(cls)) return TRUE;

    RECT r{};
    if (!::GetWindowRect(child, &r)) return TRUE;
    if (RectWidth(r) <= 0 || RectHeight(r) <= 0) return TRUE;

    if (scan->count < static_cast<int>(ARRAYSIZE(scan->spans))) {
        Span& s = scan->spans[scan->count++];
        s.start = scan->horizontal ? r.left : r.top;
        s.end = scan->horizontal ? r.right : r.bottom;
    }
    return TRUE;
}

// The rightmost position at or before `limit` where a strip of `width` fits
// without landing on another application's widget, or -1 when there is no such
// gap anywhere.
//
// Two applications using this technique both want the spot beside the clock,
// and neither can see the other's window, so without this they both take it and
// one silently covers the other.
//
// The first attempt at this parked to the left of the leftmost foreign widget,
// which is wrong whenever that widget is itself near the left edge: the
// arithmetic goes negative, clamps to zero, and drops the strip on top of the
// very thing it was avoiding. Searching for a gap is the version that actually
// holds, and it also reports honestly when there is no room at all -- which is
// a real outcome, not an error, once the strip is wider than the space left
// over.
int FindGap(const TaskbarInfo& info, bool horizontal, int width, int limit) {
    if (!info.valid || !info.window || width <= 0) return -1;

    ForeignScan scan;
    scan.horizontal = horizontal;
    ::EnumChildWindows(info.window, ForeignChildProc, reinterpret_cast<LPARAM>(&scan));

    const int barStart = horizontal ? info.bounds.left : info.bounds.top;
    const int margin = ScaleForDpi(8, info.dpi);

    // Walk leftward from the limit, jumping past each obstruction. Sixteen
    // foreign widgets is already an absurd number and the loop is bounded by
    // one pass per obstruction, so this terminates regardless of what the scan
    // found.
    int candidate = limit - width;
    for (int guard = 0; guard <= scan.count && candidate >= 0; ++guard) {
        bool clear = true;
        for (int i = 0; i < scan.count; ++i) {
            const int s = scan.spans[i].start - barStart - margin;
            const int e = scan.spans[i].end - barStart + margin;
            if (candidate < e && (candidate + width) > s) {
                candidate = s - width;   // park entirely to its left and re-test
                clear = false;
                break;
            }
        }
        if (clear) return candidate;
    }

    return -1;
}

}  // namespace

int AutoOffsetAlong(const TaskbarInfo& info, int width, int offsetFromRight) {
    if (!info.valid) return 0;

    const bool horizontal = IsHorizontal(info.edge);
    const int barLength = horizontal ? RectWidth(info.bounds) : RectHeight(info.bounds);
    const int margin = ScaleForDpi(8, info.dpi);
    const int barStart = horizontal ? info.bounds.left : info.bounds.top;

    int offset = 0;
    if (offsetFromRight >= 0) {
        offset = barLength - offsetFromRight - width;
    } else {
        // Immediately before the notification area, not at the far left. The
        // far left is where the Windows 11 weather widget lives, and the app
        // buttons beside it are centred and shift every time a window opens or
        // closes; the tray is the only part of the bar that stays put.
        int limit = barLength;
        if (info.hasNotifyArea) {
            limit = horizontal ? (info.notifyBounds.left - barStart)
                               : (info.notifyBounds.top - barStart);
        }

        limit -= margin;

        // Then find the rightmost gap in that space which another
        // application's widget is not already occupying.
        const int gap = FindGap(info, horizontal, width, limit);
        if (gap >= 0) {
            offset = gap;
        } else {
            // No room. Sitting beside the tray and overlapping is the least
            // bad outcome: it is at least where the user expects to find it,
            // and Move widget exists for the rest.
            offset = limit - width;
        }
    }


    return std::max(0, std::min(offset, std::max(0, barLength - width)));
}

COLORREF TaskbarTextColor() {
    // Deliberately not GetSysColor(COLOR_BTNTEXT). That reports the *apps*
    // theme, which is black on a default Windows 11 install, while the taskbar
    // follows the separate system theme and draws white. Using the system
    // metric would put black text on a black bar for most users.
    return TaskbarUsesLightTheme() ? RGB(0x1A, 0x1A, 0x1A) : RGB(0xFF, 0xFF, 0xFF);
}

void ForgetTaskbarTheme() { g_themePrimed = false; }

UINT RegisterTaskbarCreatedMessage() {
    return ::RegisterWindowMessageW(L"TaskbarCreated");
}

}  // namespace rc
