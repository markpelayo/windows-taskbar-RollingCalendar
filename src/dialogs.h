// dialogs.h — the small amount of modal UI the app has.
//
// All built at runtime from in-memory templates rather than from a .rc dialog
// resource, so adding a field does not mean editing a resource file and a
// header and a switch statement in three places.
//
// An accessory app is never the foreground window, so every dialog calls
// SetForegroundWindow on its owner first. Otherwise the box opens behind the
// taskbar and looks like a hang.

#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace rc {
namespace dialogs {

// One-line text input. Returns false on cancel.
bool TextInput(HWND owner,
               const std::wstring& title,
               const std::wstring& prompt,
               const std::wstring& placeholder,
               std::wstring* value);

// Two fields, for Add Calendar (name + link). A blank name is derived from the
// link by the caller.
bool TwoFieldInput(HWND owner,
                   const std::wstring& title,
                   const std::wstring& label1, const std::wstring& placeholder1,
                   const std::wstring& label2, const std::wstring& placeholder2,
                   std::wstring* value1, std::wstring* value2);

// A number in a range, for lead times and chime volume.
bool NumberInput(HWND owner,
                 const std::wstring& title,
                 const std::wstring& prompt,
                 double minValue, double maxValue,
                 bool wholeNumbers,
                 double* value);

// Date and time picker for Debug Time, in the display zone.
// Returns 0 for cancel, 1 for Simulate, 2 for Use Current Time.
int DebugTimePicker(HWND owner, const TimeZone& zone, Seconds initial, Seconds* picked);

// Yes/no with a warning icon. `destructive` makes Cancel the default button --
// Return must not be the fast path to a wipe.
bool Confirm(HWND owner,
             const std::wstring& title,
             const std::wstring& body,
             const std::wstring& confirmLabel,
             bool destructive);

void Info(HWND owner, const std::wstring& title, const std::wstring& body);
void Error(HWND owner, const std::wstring& title, const std::wstring& body);

bool OpenFile(HWND owner,
              const std::wstring& title,
              const std::wstring& filterLabel,
              const std::wstring& filterPattern,
              std::wstring* path);

bool SaveFile(HWND owner,
              const std::wstring& title,
              const std::wstring& defaultName,
              const std::wstring& filterLabel,
              const std::wstring& filterPattern,
              std::wstring* path);

}  // namespace dialogs
}  // namespace rc
