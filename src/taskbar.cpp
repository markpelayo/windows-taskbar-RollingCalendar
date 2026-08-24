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
#include <cwchar>

#include "common.h"

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
bool TaskbarUsesLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS st = ::RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (st != ERROR_SUCCESS) return false;
    return value != 0;
}

}  // namespace

TaskbarInfo QueryTaskbar() {
    TaskbarInfo info;

    const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return info;
    if (!::GetWindowRect(tray, &info.bounds)) return info;
    if (RectWidth(info.bounds) <= 0 || RectHeight(info.bounds) <= 0) return info;

    info.window = tray;
    info.dpi = DpiForWindow(tray);

    // The edge is inferred from geometry rather than asked for, because
    // SHAppBarMessage(ABM_GETTASKBARPOS) reports the primary taskbar only and
    // the shell has never had an API for "which side is this particular bar on".
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const HMONITOR mon = ::MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY);
    RECT screen{};
    if (mon && ::GetMonitorInfoW(mon, &mi)) {
        screen = mi.rcMonitor;
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

    info.valid = true;
    return info;
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
    if (!::SetParent(child, info.window)) return HostMode::Floating;
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

    ::SetWindowPos(child, (mode == HostMode::Floating) ? HWND_TOPMOST : nullptr,
                   x, y, cx, cy,
                   SWP_NOACTIVATE | (mode == HostMode::Floating ? 0u : SWP_NOZORDER));
}

int AutoOffsetAlong(const TaskbarInfo& info, int width, int offsetFromRight) {
    if (!info.valid) return 0;

    const bool horizontal = IsHorizontal(info.edge);
    const int barLength = horizontal ? RectWidth(info.bounds) : RectHeight(info.bounds);
    const int margin = ScaleForDpi(8, info.dpi);

    int offset = 0;
    if (offsetFromRight >= 0) {
        offset = barLength - offsetFromRight - width;
    } else if (info.hasNotifyArea) {
        // Immediately before the notification area, not at the far left. The
        // far left is where the Windows 11 weather widget lives, and the app
        // buttons beside it are centred and shift every time a window opens or
        // closes; the tray is the only part of the bar that stays put.
        const int notifyStart = horizontal ? (info.notifyBounds.left - info.bounds.left)
                                           : (info.notifyBounds.top - info.bounds.top);
        offset = notifyStart - margin - width;
    } else {
        offset = barLength - margin - width;
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

UINT RegisterTaskbarCreatedMessage() {
    return ::RegisterWindowMessageW(L"TaskbarCreated");
}

}  // namespace rc
