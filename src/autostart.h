// autostart.h — starting with Windows, optionally after a pause.
//
// Task Scheduler rather than an HKCU\...\Run value, for one reason: a Run entry
// cannot express a delay, and the delay is the point.
//
// Signing in is the busiest moment your disk and network will have all day, and
// a calendar widget has no business competing for it. Task Scheduler has a
// built-in "Delay task for:" on a logon trigger, which defers the launch
// itself -- not just the app's own work, which is the best a Run entry could
// manage. No shell wrapper, no admin rights, and it shows up in Task Scheduler
// where the user can see and remove it.
//
// If the registered path no longer matches this executable -- the folder was
// moved -- the entry is rewritten on the next launch rather than left dangling.

#pragma once

#include <string>

#include "common.h"

namespace rc {
namespace autostart {

bool IsEnabled();
int DelaySeconds();            // 0 when disabled

// Registers or updates the logon task. Returns false with `error` filled in
// when policy on the machine forbids it, so the menu can say so rather than
// silently failing.
bool Enable(int delaySeconds, std::wstring* error);
bool Disable(std::wstring* error);

// Called at launch: rewrites the task when the stored path is stale.
void RepairIfMoved();

std::wstring Describe();       // "Off", "On", "After 20 s"

extern const wchar_t* const kTaskName;   // L"RollingCalendar"

}  // namespace autostart
}  // namespace rc
