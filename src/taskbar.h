// taskbar.h — putting a window inside the Windows taskbar.
//
// This is the one genuinely unsupported thing the app does, so it deserves an
// honest explanation.
//
// macOS gives Rolling Calendar an NSStatusItem: a documented slot in the menu
// bar of whatever width you ask for, which is exactly what a scrolling timeline
// with two text gutters needs. Windows has no equivalent. A notification-area
// icon is a fixed square, 16x16 at 100% scaling, and cannot show text -- which
// would lose the timeline and both gutters, i.e. the entire app. Deskbands, the
// old COM mechanism for real taskbar toolbars, were deprecated in Windows 8 and
// their UI was removed in Windows 11.
//
// What still works is to create an ordinary child window and re-parent it into
// the taskbar's own window (class "Shell_TrayWnd") with SetParent. The child
// then moves, hides and auto-hides along with the taskbar, because as far as
// the shell is concerned it is part of it.
//
// Written from the public documentation for FindWindow, SetParent and
// SetWindowPos. Being undocumented behaviour, it can fail -- a Windows update,
// a shell replacement, a locked-down machine -- so every failure path falls
// back to a floating always-on-top window positioned over the taskbar. Less
// integrated, but never invisible.

#pragma once

#include <windows.h>

namespace rc {

enum class HostMode {
    Embedded,   // a child of Shell_TrayWnd; moves and hides with the taskbar
    Floating    // topmost window positioned over the taskbar
};

enum class TaskbarEdge { Bottom, Top, Left, Right };

struct TaskbarInfo {
    HWND window = nullptr;
    RECT bounds{};
    TaskbarEdge edge = TaskbarEdge::Bottom;
    int dpi = 96;
    bool valid = false;

    // The notification area ("TrayNotifyWnd"): clock, chevron, volume, network.
    // The widget parks immediately to its left, because that is the only part
    // of the taskbar that stays put -- app buttons are centred on Windows 11
    // and move as windows open and close, and the far left is where the weather
    // widget lives.
    RECT notifyBounds{};
    bool hasNotifyArea = false;
};

TaskbarInfo QueryTaskbar();

// Thickness available to a widget inside the taskbar, in physical pixels, with
// a small margin so the strip does not touch the edge.
int UsableThickness(const TaskbarInfo& info);

HostMode EmbedInTaskbar(HWND child, const TaskbarInfo& info);
void DetachFromTaskbar(HWND child);

// Places `child` along the taskbar. In Embedded mode the coordinates are
// relative to the taskbar window, in Floating mode they are screen coordinates;
// callers do not need to care which.
void PositionWidget(HWND child,
                    const TaskbarInfo& info,
                    HostMode mode,
                    int offsetAlong,
                    int width,
                    int thickness);

// Where the widget should sit: `offsetFromRight` px from the taskbar's right
// edge when the user has dragged it, otherwise immediately left of the tray.
int AutoOffsetAlong(const TaskbarInfo& info, int width, int offsetFromRight);

// The colour the shell draws taskbar text in.
//
// GetSysColor(COLOR_BTNTEXT) is wrong here: it reports the *apps* theme, which
// is black under a default Windows 11 setup, while the taskbar follows the
// separate system theme and draws white. Reading the theme preference directly
// is the only way to match what is beside us.
COLORREF TaskbarTextColor();

// The shell broadcasts "TaskbarCreated" to every top-level window when Explorer
// restarts. That is the moment an embedded widget silently loses its parent.
UINT RegisterTaskbarCreatedMessage();

}  // namespace rc
