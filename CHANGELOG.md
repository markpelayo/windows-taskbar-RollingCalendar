# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-08-25

Initial release. A C++/Win32 port of [macos-menubar-RollingCalendar](https://github.com/markpelayo/macos-menubar-RollingCalendar) v1.4.2, written against a functional specification of that app rather than translated line by line from the Swift.

### Added

**The strip**

- A timeline widget hosted inside the Windows taskbar, drawing today's calendar as coloured capsules flowing right to left past a fixed red now line at the centre of the strip.
- Blocks fade from the left as they elapse, so the proportion of a block already spent is readable without any text.
- Left and right text gutters naming the current block with its time remaining, and the next block with its duration. The left gutter turns red and bold in the last two minutes of a block.
- Overlap badges in both gutters. A badge on the right means the clash is still ahead; it crosses to the left as the clash reaches the now line, so the badge's side reports when the clash is, not only that there is one.
- Visible span selectable from ±5 minutes to ±2 hours, strip width from 100 to 450 points, label length from 100 to 480 points. Labels are shortened with an ellipsis; countdowns and overlap badges are never cut.
- All-day and zero-length events are excluded from the strip and shown only in the dropdown.
- Hidden settings for the now-line width, urgent threshold, unmatched colour, solid or translucent blocks, block gap, corner radius, font size and day anchor keyword.

**Taskbar hosting**

- A child window re-parented into `Shell_TrayWnd`, so the strip moves, hides and auto-hides with the taskbar. Written from the public documentation for `FindWindow`, `SetParent` and `SetWindowPos`.
- Automatic placement immediately left of the notification area, which is the only part of the taskbar that stays put, plus drag-to-reposition and a reset.
- Re-embedding on the `TaskbarCreated` broadcast, so an Explorer restart does not permanently detach the widget.
- Periodic re-checking of the taskbar's position, edge and DPI, since the shell does not announce any of them.
- A floating always-on-top fallback on every failure path, so the widget is never invisible.
- A notification-area icon carrying the same menu and a tooltip with the same text as the gutters, as a guaranteed route to the app when the strip cannot be reached.

**Calendar data**

- A hand-rolled RFC 5545 parser covering `FREQ=DAILY|WEEKLY|MONTHLY|YEARLY` with `INTERVAL`, `BYDAY`, `BYMONTHDAY`, `UNTIL` and `COUNT`, plus `EXDATE` and `RECURRENCE-ID` overrides. Recurrence is expanded per requested day; non-recurring events are filtered to the four loaded days as they are parsed.
- Link normalisation accepting Google Calendar embed links, "add by URL" links, `webcal://` links, any URL ending `.ics`, a bare calendar address, and `file://` paths to local `.ics` files.
- Display time zone taken from a link's `ctz=` parameter where present, resolved through a CLDR-derived IANA-to-Windows mapping table, falling back to the machine's zone on a miss.
- Feed fetching over WinHTTP with a 20-second timeout and the local cache bypassed, every five minutes, at launch, on wake from sleep and on any source change. Stale responses are dropped by generation token.
- A 30-minute failure grace period: the last good day survives underneath the error message before being cleared.
- Named calendar profiles, switched with a click. One feed is read at a time; profiles do not merge.
- A built-in demo calendar with sixteen blocks a day and two deliberate collisions, shown until a calendar is configured.

**Colour**

- Keyword rules matched whole-word against event titles, longest phrase winning, since iCalendar feeds carry no colour of their own.
- A forgiving CSV importer: BOM stripping, line-ending normalisation, delimiter auto-detection, quoted fields, header identification by name and column inference when there is no header row, with diagnostics on failure.
- Seventeen named colours plus hex values.
- A built-in 42-rule, 6-category sample, seeded once and guarded by a flag so that cleared stays cleared, exportable as CSV.

**The menu**

- The complete interface in one menu, rebuilt from scratch on every open. Owner-drawn rows for the day's blocks, captions and keyword categories; ordinary menu text everywhere else, so themes, high contrast and screen readers work without special handling.
- An anchor-to-anchor day list, capped at 60 rows with date separators, aligned by computed tab stops rather than by padding, showing a marker and bold title for the block in progress, plus duration, category swatch and overlap badge per row.
- An ISO 8601 week-number caption and a freshness caption measured on the real clock.
- Debug Time: an offset rather than a freeze, so the simulated clock keeps running and countdowns still tick.

**Sound**

- Time Block Alerts with lead times as a set rather than a single choice, a category filter, mutually exclusive sound and speech output, SAPI 5 voices, sounds from `%WINDIR%\Media` plus imports, and a lateness rule that marks an alert more than thirty seconds late as fired and skips it silently.
- The Westminster Quarters synthesised from inharmonic sine partials, with an optional hour strike and adjustable volume. Nothing is bundled and nothing is downloaded: a recording of Big Ben is copyright, the 1793 tune is not.
- A single Sound Hours schedule gating both features, taking a set of windows, with inclusive endpoints, midnight wrapping and an explicit all-day option.

**Housekeeping**

- Settings in one INI file at `%APPDATA%\RollingCalendar\settings.ini`, cached in memory and written through on change. Restore Defaults deletes the whole file rather than a list of keys.
- Run at Startup through a Task Scheduler logon task with a native delay of 5 to 60 seconds, rather than a registry Run entry, which cannot express one. The task is rewritten if the executable has moved.
- One second timer driving the redraw, the re-measure, the alert check and the chime check. Hysteretic resizing so the taskbar does not relayout for a single pixel.
- Every GDI handle owned by an RAII wrapper, fonts and colours cached per DPI, no child processes and no iostreams.
- `build.bat` for a single MSVC invocation and `CMakeLists.txt` for editors and CI, producing the same statically linked binary. No vcpkg, no NuGet, no submodules and no third-party libraries.
- A GitHub Actions workflow building both paths, checking the binary launches and stays up, and attaching a zip to tagged releases.

### Not yet verified on hardware

This release has not been run on a physical machine by the author, and no configuration has been observed in use. If a binary is attached below, it is the one CI produced: the workflow compiles both build paths, then launches the executable, waits several seconds and confirms it is still running before packaging anything. That is the only evidence there is. It rules out a failure to compile or link and a crash on startup. It says nothing at all about whether the strip appears in the right place, whether the timeline is drawn correctly, or whether any of the sound features work.

Specifically unverified: **Windows 10 of any build, fractional display scaling, taskbars docked to the left or right edge, and multi-monitor setups.** The taskbar embedding also depends on undocumented shell behaviour that Microsoft has never committed to and may change at any time.

Treat 1.0.0 as a first cut that compiles, not as tested software.

[1.0.0]: https://github.com/markpelayo/windows-taskbar-RollingCalendar/releases/tag/v1.0.0
