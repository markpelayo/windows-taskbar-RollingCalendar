// diag.h — a temporary diagnostic log.
//
// This file exists to answer one question: why does a child of Shell_TrayWnd
// draw underneath the taskbar on some machines when the same technique works
// for another application on the same taskbar. It is not a logging framework
// and it is not meant to survive. Delete this module, its two call sites in
// main.cpp and the diag:: lines elsewhere once the placement and z-order
// behaviour is understood.
//
// Verbosity is deliberately high but bounded: everything at startup, a
// heartbeat every ten seconds, and a full snapshot whenever the menu is opened,
// which gives the user a way to mark the moment something looked wrong. That
// works out at a couple of kilobytes a minute, and the file is truncated on
// every launch so it cannot grow without limit across sessions.

#pragma once

#include <windows.h>

#include <string>

namespace rc {
namespace diag {

// Opens the log beside the executable. Falls back to %APPDATA%\RollingCalendar
// when that folder is not writable, which is the normal case for anything
// unpacked into Program Files.
void Open();
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
