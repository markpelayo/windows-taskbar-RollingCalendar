# Contributing

This is a small project with one author and no roadmap. Contributions are welcome; the most valuable ones are probably not code.

## Building

The only prerequisite is MSVC and the Windows SDK. Visual Studio 2022 Community or the standalone Build Tools both provide them. There is no package manager step, nothing to restore and nothing to download.

Open **x64 Native Tools Command Prompt for VS 2022** and run:

```
build.bat
```

That produces `build\RollingCalendar.exe`, statically linked, with no redistributable dependency.

If you prefer CMake:

```
cmake -B build-cmake -S . -A x64
cmake --build build-cmake --config Release
```

Both are built in CI on every push. `build.bat` is the canonical build — CI runs it rather than an approximation of it, so that the thing being tested is the thing contributors actually use — and the CMake pass exists because a build file nobody exercises is a build file that has already broken.

## Reporting a bug

The single most useful thing you can send is a report from a configuration the author has not been able to test. As of 1.0.0 that list is: **Windows 10 of any build, fractional display scaling, a taskbar docked to the left or right edge, and multi-monitor setups.** Those are exactly the cases where a widget that positions itself by measuring the taskbar is most likely to be wrong, and there is currently no evidence at all about how it behaves in them. A report saying "this works fine on Windows 10 22H2 at 125% with two monitors" is worth as much as a report of a fault.

A useful report contains:

- **Your Windows version and build number.** Press Win+R, type `winver`, and copy what it says. "Windows 11" on its own is not enough — the taskbar has changed several times within Windows 11.
- **Display scaling.** 100%, 125%, 150%, and whether it is the same on every monitor.
- **Taskbar position** — bottom, top, left or right — and whether auto-hide is on.
- **How many monitors**, and which one the taskbar is on.
- **Whether the strip was embedded or floating.** An embedded strip slides and hides with the taskbar; a floating one sits over it and stays put. If the strip is missing entirely, say whether the notification-area icon is present, because that distinguishes "the app is not running" from "the app is running and the strip did not appear".
- **What the calendar link looks like, with the private parts removed.** Not the link itself — replace the calendar identifier with `xxxx` and keep the shape. `https://calendar.google.com/calendar/ical/xxxx%40group.calendar.google.com/public/basic.ics` tells us everything we need and nothing we should not have.
- **The contents of `%APPDATA%\RollingCalendar\settings.ini`.** It is plain text, it contains no credentials, and it is usually the fastest route to reproducing what you are seeing. Redact the calendar links in it on the same principle as above.

For a rendering fault, a screenshot of the taskbar is worth several paragraphs. For a parsing fault, the smallest `.ics` file that reproduces it is worth more than any description.

## Code style

- **C++17.** Not later; MSVC's C++20 support was still uneven when this was written and there is nothing here that needs it.
- **Win32 only.** No MFC, no ATL, no WinRT, no WIL.
- **No third-party libraries, no vcpkg, no NuGet, no submodules.** Cloning this repository and building it must need a compiler and nothing else. That property is worth more than any convenience a dependency would buy, and pull requests that break it will not be merged regardless of how good the library is. `src/raii.h` is about eighty lines of what WIL would give you, kept in-tree for exactly this reason.
- **No iostreams.** They add a static initialiser and a good deal of binary to a program with no console that never prints anything.
- **Every GDI handle is owned by an RAII wrapper.** Use the types in `src/raii.h`. A process is capped at 10,000 GDI handles, and this app repaints once a second, so a single leaked brush per redraw kills it in under three hours. There are no bare `CreateBrush`/`DeleteObject` pairs in the codebase and there should not be a first one.
- **Comments explain why, not what.** `// increment the counter` is noise. `// stamped before the five-second check, so a late tick consumes that quarter permanently` is the reason the code is shaped the way it is, and is the only kind of comment worth the line it occupies. The headers in `src/` carry the design rationale; if a change alters the reasoning, change the header comment too.
- **Every parse is total.** Nothing arriving from a feed, a CSV or a settings file may be assumed well-formed. Check ranges, bound durations, and prefer returning nothing over returning something wrong.
- Four-space indent, 100-column soft limit, `lowerCamelCase` locals, `PascalCase` functions, `kConstantName` for constants, trailing underscore on private members. Match the surrounding file if in doubt.

## Pull requests

Keep them small and keep them focused. Say in the description what the change is for, not just what it does.

If a change adds a user-visible behaviour, it needs a menu item or it needs to be documented in the hidden-settings table in the README; a feature nobody can find is not a feature. If it changes something the README describes, update the README in the same pull request.

And to say it once more, because it is the one thing this project will not compromise on: **the zero-dependency property must survive**.
