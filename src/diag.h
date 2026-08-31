// diag.h — an opt-in diagnostic log.
//
// This was written to answer one question: why does a child of Shell_TrayWnd
// draw underneath the taskbar on some machines when the same technique works
// for another application on the same taskbar. The answer turned out to be the
// Windows 11 composition island, and it took a log to find, because every
// diagnostic the app could report said the window was correct.
//
// It stays for exactly that reason. The taskbar is somebody else's window and
// its internals are not contractual, so the next machine where the strip does
// not appear will need the same evidence, and "install a debugger" is not a
// reasonable thing to ask of anyone. It is off unless `diagnosticLog=1` is set
// under [hidden] in settings.ini, so nobody pays for it who is not debugging.
//
// When it is on, verbosity is high but bounded: everything at startup, a
// heartbeat every ten seconds, and a full snapshot whenever the menu is opened,
// which gives the user a way to mark the moment something looked wrong. That
// works out at a couple of kilobytes a minute, and the file is truncated on
// every launch so it cannot grow without limit across sessions.

#pragma once

#include <windows.h>

#include <string>

namespace rc {
namespace diag {

// Opens the log beside the executable, falling back to
// %APPDATA%\RollingCalendar when that folder is not writable, which is the
// normal case for anything unpacked into Program Files.
//
// Does nothing unless `enabled`. Every other function in this file is a no-op
// while the log is closed, so the call sites do not need to test anything.
void Open(bool enabled);
void Close();

bool IsOpen();
const std::wstring& Path();

void Log(const wchar_t* fmt, ...);

// Everything known about a window: handle, class, style, extended style,
// parent, rect, visibility.
void LogWindow(const wchar_t* label, HWND hwnd);

// The taskbar's children in z-order, front to back, with our own marked. This
// is the measurement that matters: if the strip is at the front and still
// invisible, the cause is not the sibling order.
void LogZOrder(HWND tray, HWND self);

// A full picture, tagged with why it was taken.
void Snapshot(const wchar_t* reason, HWND self, HWND tray);

}  // namespace diag
}  // namespace rc
