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

#include <string>
#include <vector>

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

    // True when the shell renders its taskbar through a composition island: a
    // "Windows.UI.Composition.DesktopWindowContentBridge" child spanning the
    // whole bar, which is how Windows 11 draws it.
    //
    // This matters more than it sounds. A composited visual is drawn above the
    // GDI painting of sibling windows whatever the legacy z-order says, so an
    // ordinary child window sitting at the very front of the taskbar's children
    // is still underneath the bar's own pixels. Winning the child-order fight
    // and then losing to the compositor looks, from the outside, exactly like
    // the widget not running at all.
    //
    // The way out is to be composited too -- see MakeCompositedChild below.
    bool compositedShell = false;

    // Which display this bar belongs to. The adapter device name (\\.\DISPLAY2)
    // rather than an index, because indices are reassigned when a display is
    // unplugged, and a stored preference that quietly comes to mean a different
    // screen after a dock is worse than one that fails to apply.
    std::wstring monitorDevice;
    bool isPrimaryMonitor = false;

    // "Display 2 - 1920 px wide (primary)". For the menu.
    std::wstring MonitorLabel() const;
};

// Every taskbar the shell currently has: the primary one plus a per-monitor bar
// for each display, when "Show my taskbar on all displays" is on. Primary
// first, then left to right, so the menu's order is stable between openings.
//
// A single entry does not mean a single monitor -- it means the user has the
// taskbar on one screen only, which is a different thing and not something to
// argue with.
std::vector<TaskbarInfo> EnumerateTaskbars();

// The bar to host the strip in: the one on `preferredDevice` if that display
// still has a taskbar, otherwise the primary, otherwise whatever exists.
//
// Falling back rather than failing is deliberate. A laptop returning from a
// dock has lost the display its preference names, and the right response is to
// show the strip somewhere the user can see it and leave the preference alone
// so it reapplies when the dock comes back.
TaskbarInfo QueryTaskbar(const std::wstring& preferredDevice = std::wstring());

// Turns an embedded child into a layered window, so DWM composites it as its
// own visual and it lands above the shell's composition island.
//
// WS_EX_LAYERED on a child window is supported from Windows 8 onwards. Every
// reference that says otherwise, including an earlier comment in this project,
// predates that. The window stays fully opaque: alpha is only the mechanism for
// getting redirected into the compositor, not an effect.
//
// Returns false when the call fails, which is the caller's cue to fall back to
// a floating window rather than to a widget nobody can see.
bool MakeCompositedChild(HWND child);

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
// edge when the user has dragged it, otherwise immediately left of the tray --
// or left of any other application's widget already embedded there, since two
// apps using this technique would otherwise both claim the same spot and one
// would silently cover the other.
int AutoOffsetAlong(const TaskbarInfo& info, int width, int offsetFromRight);

// Re-asserts the strip's place at the top of the taskbar's child order.
//
// A child inserted by SetParent lands at the bottom, where the shell's own
// content draws over it, and the shell reorders its children whenever it
// relayouts. So this is called on every move and again on the periodic poll,
// rather than once at embedding time.
void RaiseWithinTaskbar(HWND child);

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
