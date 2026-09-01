# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-09-01

A follow-up to the first release: the strip's height is now a setting, the menu reports what is in force rather than requiring its submenus to be opened, the diagnostic log added to find one fault has been removed now that the fault is found, and the review pass that preceded 1.0.0 has been carried through into genuine fixes.

### Added

- A `Timeline Height` submenu directly below Text Size, setting the height of the coloured band: 16, 20, Default (24 px), 28 and 32, plus one custom from 8 to 64 px, edited in place. The default keeps its place in the ascending order rather than being lifted to the top, because the list is a scale and a scale with a rung out of sequence is harder to read than one with a word on a rung. The band is trimmed to fit when the taskbar is shorter than the chosen height. It is part of the strip's geometry, so it sits below the separator with the rest of it, and Reset Strip Settings clears it.
- The geometry submenus report the value in force in their own titles — `Time Range: ±1 hour`, `Timeline Width: 250 pt`, `Label Length: 360 pt`, and the same for Text Size and Timeline Height — with a `(default)` marker on the default row inside. The setting in force should be readable from the parent row rather than requiring the submenu to be opened and its ticks scanned. Labels is the exception: four independent toggles have no single value to report.
- Saved Calendars is ticked in the top-level menu when a saved calendar rather than the demo is the source, so which of the two rows is live can be read at a glance.
- Run at Startup is ticked when it is enabled, and gains an explicit `On (no delay)` row alongside `Off` and the six delays. Exactly one row in the submenu carries the tick at any time: while startup is off, every delay row reads unticked, which is the honest picture, since none of them is in force. The tick reflects the registered scheduled task rather than the stored flag, so deleting the task in Task Scheduler is not reported back as still on.
- A faint highlight behind the day list's current row, in addition to the marker and the bold title. Three cues rather than two, because a marker alone is easy to miss in a list of sixty.

### Changed

- Text Size now offers one editable custom size rather than a growing list of removable ones. Only one size is ever in force, so the second entry in such a list is dead weight, and a list you can add to but not take from fills up permanently.
- The simulated-clock marker reads `! Simulated !`, with the spaces, in red and bold, in both the caption row and the strip itself. The same words drawn the same way in both places, so the two cannot be read as different states.
- The settings file has changed shape. The `[fontsize]` section is gone, and `blockHeight` has moved out of `[hidden]` into `[strip]`, joining `titleFontSize`, `customFontSize` and `customBlockHeight`, because each of them now has a menu item and each resets with the rest of the strip's geometry. A 1.0.0 settings file still loads; the one wrinkle is that a `blockHeight` set by hand under `[hidden]` is now ignored and falls back to the default, and must be set again from the Timeline Height submenu or moved to `[strip]`.

### Removed

- The diagnostic log, its `diagnosticLog` setting and every call site. It was added to find one specific fault — the Windows 11 composition island, which every other signal reported as working — and it found it. A file written beside the executable that exists to answer a question already answered is not earning its place. `hostOverride` remains as the escape hatch for the case the log was written for: a strip that cannot be seen.

### Fixed

**Behaviour**

- A failed fetch is now retried after fifteen seconds, doubling on each further failure up to the ordinary five-minute interval, instead of always waiting out the full five minutes. A failure at launch is almost always a network stack that has not finished coming up at log on, and it clears in seconds; the old behaviour turned that into five minutes of "Calendar unreachable" on a machine that was in fact online. The doubling is there so a feed that is genuinely gone is not hammered.
- Both fetch paths — success and failure — now re-lay-out immediately rather than waiting for the next tick. The labels have just changed from nothing, or from an error, to a full day, and until the widget is resized to match, the window is the wrong width for what is being drawn into it.
- The timeline is now given its space before the gutters get theirs. The labels will otherwise consume the whole width, and the strip could collapse to two overlapping labels with no blocks between them, which is the one failure mode that leaves the widget saying nothing at all.

**Correctness, leaks and per-tick cost**

- A use-after-free when removing the last saved calendar profile.
- A self-aliasing bug that left `activeProfile` pointing at a stale name after renaming the calendar in use. Both it and the fix above come from the same cause: the name being passed in lived inside the vector the function then rewrote or erased, so it is now taken by value rather than by reference.
- An owner-draw payload leaked on one early-return path when building a menu row.
- The taskbar theme registry read was happening on every one of the several calls a tick makes, four times a second. It is now memoised and invalidated outright on the theme-change message, so a theme switch is still picked up at once.
- The tray tooltip was composed every second and the result thrown away unchanged on four ticks in five. It is now composed genuinely every five seconds.

### What has and has not been verified

Run, and working, on **Windows 11 24H2, build 26100**, on one machine: at 3440x1440 and at 1920x1080, on a bottom-docked taskbar at 100% scaling, in both the light and dark system themes, and alongside the author's [windows-taskbar-pinger](https://github.com/markpelayo/windows-taskbar-pinger) embedded in the same taskbar. The strip embeds, draws, positions itself around the other widget, survives an Explorer restart and reads a real Google Calendar feed.

Not verified at all: **Windows 10 of any build, fractional display scaling, taskbars docked to the left or right edge, auto-hide taskbars, more than one taskbar, and ARM64.** Those are the configurations where a widget that positions itself by measuring the taskbar is most likely to be wrong, and reports from them — the Windows build number, the display scaling, and which `hostOverride` value produced a visible strip — are the most useful thing anyone could send.

The code has had a correctness, leak and performance audit, and the Fixed section above lists what it turned up. It has never been run under a profiler or a leak detector. So the claims made here about memory and per-tick cost are claims about what the design guarantees, not about anything that has been measured, and no figure for either is quoted because none exists.

## [1.0.0] - 2026-09-01

Initial release. A C++/Win32 port of [macos-menubar-RollingCalendar](https://github.com/markpelayo/macos-menubar-RollingCalendar) v1.6.3, written against a functional specification of that app rather than translated line by line from the Swift.

Everything below is new, because there is nothing before it. It is grouped by area rather than listed flat; a two-hundred-bullet list of a first release tells you less than a paragraph would.

### Added

**The strip**

- A timeline widget hosted inside the Windows taskbar, drawing today's calendar as coloured capsules flowing right to left past a fixed red now line at the centre of the strip. No tick marks and no gridlines: the question it answers is "where am I", not "what time is it".
- Blocks fade from the left as they elapse, so the proportion of a block already spent is readable without any text.
- Left and right text gutters naming the current block with its time remaining, and the next block with its duration. The left gutter turns red and bold in the last two minutes of a block.
- Overlap badges in both gutters. A badge on the right means the clash is still ahead; it crosses to the left as the clash reaches the now line, so the badge's side reports *when* the clash is, not only that there is one.
- A black ring around any block drawn on top of a longer one. Two concurrent blocks of the same colour would otherwise merge into a single capsule, leaving the gutter badge to report a clash without showing where it was. Only the upper block is ringed; ringing both would read as three blocks rather than two.
- A `Text Size` submenu: the shell's own menu-font size by default, so the strip matches everything else in the bar, plus 11, 13 and 15 pt presets and a list of custom sizes from 6 to 48 pt, added as they are typed and removable one at a time. The typed size is hidden from the list when it coincides with a preset, because a radio group with two marks in it is a lie about what is in force.
- `Ending Soon Flash`, ported from the macOS version 1.6.3: for the last N minutes of the running block, its name in the left gutter blinks red once a second. Off by default, with presets of one, two and five minutes and a custom value from a quarter of a minute to sixty. The label's weight changes once at the start of the window rather than with the blink, so it cannot jitter or resize as it flashes.
- Reset Strip Settings covers the strip's geometry only — time range, timeline width, the label toggles, label length, and text size with its customs. It deliberately leaves Ending Soon Flash alone; that is a warning, not a proportion, and a click aimed at the timeline's dimensions should not silently switch off the thing telling you a meeting is about to end. Restore Defaults clears it.
- All-day and zero-length events are excluded from the strip and shown only in the dropdown.

**Taskbar hosting**

- A child window re-parented into `Shell_TrayWnd`, so the strip moves, hides and auto-hides with the taskbar. Written from the public documentation for `FindWindow`, `SetParent`, `SetWindowPos`, `SetWindowLongPtr` and `SetLayeredWindowAttributes`. The re-parenting is confirmed with `GetParent` rather than trusted from `SetParent`'s return value, which is null both for failure and for a window that had no parent.
- The strip is a **layered** child (`WS_EX_LAYERED`). This is what makes it visible at all on Windows 11: the shell renders its taskbar through a composition island, and DWM draws a composited visual above the GDI painting of sibling windows regardless of the legacy child z-order, so an ordinary child sitting at the very front of the taskbar's children is still underneath the bar's own pixels. Being layered gets the window redirected into a composition visual of its own, above the island. `WS_EX_LAYERED` on a child window has been supported since Windows 8; a great many references, including an earlier comment in this project's own source, say otherwise, and that assumption cost a day.
- Real transparency, as a consequence of the above. A colour key on the layered window makes the strip's background see-through to the taskbar rather than an approximated slab. Guessing the bar's colour was never going to work: with transparency effects on it is acrylic over the user's wallpaper and there is no colour to guess. The key is defined once, in `timeline::ChromaKey()`, and read by both the window setup and the paint code, so the two cannot disagree.
- Automatic placement immediately left of the notification area, which is the only part of the taskbar that stays put, and to the left of any other application's widget already embedded there. Two apps using this technique would otherwise both claim the same spot and one would silently cover the other. When there is no clear gap it overlaps deliberately, which is at least where the widget would be looked for; `Move widget…` exists because from the outside a deliberate overlap and a placement bug look identical.
- `Move widget…` and `Reset widget position`, storing the position as a distance from the taskbar's right edge.
- `Show on Display`, a picker over every taskbar the shell currently has. It only appears when there is more than one, since a menu offering a choice of one is a puzzle rather than a setting. The choice is stored as the adapter device name rather than an index, because indices are reassigned when a display is unplugged and a preference that quietly comes to mean a different screen after a dock is worse than one that fails to apply. A missing display falls back to the primary and leaves the preference intact.
- Re-embedding on the `TaskbarCreated` broadcast, so an Explorer restart does not permanently detach the widget, and re-assertion of the strip's place in the child order on every move and on the periodic poll, since the shell reorders its children whenever it relayouts.
- Periodic re-checking of the taskbar's position, edge and DPI, since the shell announces none of them. The edge is inferred from geometry, because `SHAppBarMessage(ABM_GETTASKBARPOS)` reports the primary bar only.
- A floating always-on-top fallback on every failure path — taskbar not found, `SetParent` failed, layering failed, shell replaced — so the widget is never invisible.
- A notification-area icon carrying the same menu and a tooltip with the same text as the gutters, as a guaranteed route to the app when the strip cannot be reached.
- `hostOverride` in the settings file to force the choice: plain child, layered child or floating. The failure it works around is a widget that cannot be seen, and the alternative to a one-line edit in an INI file would have been a rebuild.

**Calendar data**

- A hand-rolled RFC 5545 parser covering `FREQ=DAILY|WEEKLY|MONTHLY|YEARLY` with `INTERVAL`, `BYDAY`, `BYMONTHDAY`, `UNTIL` and `COUNT`, plus `EXDATE` and `RECURRENCE-ID` overrides. Recurrence is expanded per requested day; non-recurring events are filtered to the four loaded days as they are parsed.
- Link normalisation accepting Google Calendar embed links, "add by URL" links, `webcal://` links, any URL ending `.ics`, a bare calendar address, and `file://` paths to local `.ics` files.
- Display time zone taken from a link's `ctz=` parameter where present, resolved through a CLDR-derived IANA-to-Windows mapping table, falling back to the machine's zone on a miss.
- Feed fetching over WinHTTP with a 20-second timeout and the local cache bypassed, every five minutes, at launch, on wake from sleep and on any source change. Stale responses are dropped by generation token. The fetch runs on a worker thread that exists only for the duration of the download and then exits.
- A 30-minute failure grace period: the last good day survives underneath the error message before being cleared.
- Named calendar profiles, switched with a click. One feed is read at a time; profiles do not merge.
- A built-in demo calendar with a time-blocked day and deliberate collisions, shown until a calendar is configured, so the overlap badges can be seen before you have any overlaps of your own.

**Colour**

- Keyword rules matched whole-word against event titles, longest phrase winning, since iCalendar feeds carry no colour of their own.
- A forgiving CSV importer: BOM stripping, line-ending normalisation, delimiter auto-detection, quoted fields, header identification by name and column inference when there is no header row, with diagnostics on failure naming the delimiter it detected, the columns it assigned and the first data row.
- Seventeen named colours plus hex values.
- A built-in 42-rule, 6-category sample, seeded once and guarded by a flag so that cleared stays cleared, exportable as CSV.

**The menu**

- The complete interface in one menu, rebuilt from scratch on every open. Owner-drawn rows for the day's blocks, captions and keyword categories; ordinary menu text everywhere else, so themes, high contrast and screen readers work without special handling.
- Ordering matched to the macOS version 1.6.3. Above the separator sits what the blocks *are* — Demo Calendar, Saved Calendars, Keyword Colors, Ending Soon Flash. Below it sits geometry and nothing but geometry — Time Range, Timeline Width, Labels, Label Length, Text Size, Reset Strip Settings — which is what makes that reset a safe click. Keyword Colors used to sit with the geometry, and it is about the events rather than the timeline's proportions.
- "Restore Strip Settings" renamed to "Reset Strip Settings". Two rows in one menu both beginning "Restore" read as the same action twice, and the heavier of the two is the one further down.
- An anchor-to-anchor day list, capped at 60 rows with date separators, aligned by computed tab stops rather than by padding. The block in progress carries a marker and a bold title. Duration, category swatch and overlap badge per row.
- An ISO 8601 week-number caption and a freshness caption measured on the real clock rather than the simulated one.
- Debug Time: an offset rather than a freeze, so the simulated clock keeps running and countdowns still tick. While it is active the caption says the clock is simulated and the strip carries its own marker in red, so a strip showing a time that is not the time cannot be mistaken for one that is.

**Sound**

- Time Block Alerts with lead times as a set rather than a single choice, a category filter, mutually exclusive sound and speech output, SAPI 5 voices, sounds from `%WINDIR%\Media` plus imports, and a lateness rule that marks an alert more than thirty seconds late as fired and skips it silently. Announcing "ten minutes before" when there are three minutes left is not a late alert, it is a wrong one.
- The Westminster Quarters synthesised from inharmonic sine partials, with an optional hour strike and adjustable volume. Nothing is bundled and nothing is downloaded: a recording of Big Ben is copyright, the 1793 tune is not. Rung quarters are recorded by absolute index rather than wall-clock time, so a daylight-saving fall-back does not silence the repeated hour.
- A single Sound Hours schedule gating both features, taking a set of windows, with inclusive endpoints, midnight wrapping and an explicit all-day option.

**Housekeeping**

- Settings in one INI file at `%APPDATA%\RollingCalendar\settings.ini`, cached in memory and written through on change. Restore Defaults deletes the whole file rather than a list of keys, so a setting added in a later version cannot be left behind by an older reset. The text size in force is a `[strip]` key; the list of sizes the user has added lives in its own `[fontsize]` section, counted and numbered like every other list in the file.
- Hidden settings under `[hidden]` for the now-line width, urgent threshold, unmatched colour, solid or translucent blocks, block gap, block height, corner radius, day anchor keyword, host mode override, past-block fade and the gap between a gutter and the timeline. They have no menu item because each of them is a decision that was made once, and a menu row for every one of them would bury the settings that are actually worth changing.
- An opt-in diagnostic log, off unless `diagnosticLog=1` is set by hand under `[hidden]`, writing `RollingCalendar-log.txt` beside the executable. The taskbar hosting fails in ways that leave nothing on screen to look at, so there has to be some route to a record of what the shell did.
- Run at Startup through a Task Scheduler logon task with a native delay of 5 to 60 seconds, rather than a registry Run entry, which cannot express one. Signing in is the busiest moment the machine will have all day and a calendar widget has no business competing for it. The task is rewritten if the executable has moved.
- One window and one one-second timer, driving the redraw, the re-measure, the alert check and the chime check. Nothing else is scheduled. Hysteretic resizing so the taskbar does not relayout for a single pixel.
- Nothing allocated on the repaint path: every GDI handle owned by an RAII wrapper, with brushes, pens, fonts and the back buffer cached and rebuilt only when a colour, the DPI or the size changes. A process is capped at 10,000 GDI handles, so one leaked brush per redraw would kill the app in under three hours.
- Events held in memory only and never written to disk; no child processes, no iostreams, no third-party libraries and no runtime to install.
- `build.bat` for a single MSVC invocation and `CMakeLists.txt` for editors and CI, producing the same statically linked binary. No vcpkg, no NuGet, no submodules and no third-party libraries.
- A GitHub Actions workflow building both paths, checking the binary launches and stays up, and attaching a zip to tagged releases.

### A caveat that testing cannot settle

**The taskbar embedding relies on undocumented shell behaviour.** Every call it makes is documented and supported, but nothing in the documentation says the taskbar will tolerate a foreign child window living inside it, and Microsoft has never committed to it. That behaviour already changed once, between the Windows 10 taskbar and the Windows 11 composition island, and the change was invisible until it was investigated. It works here; it is not promised to keep working anywhere.

[1.1.0]: https://github.com/markpelayo/windows-taskbar-RollingCalendar/releases/tag/v1.1.0
[1.0.0]: https://github.com/markpelayo/windows-taskbar-RollingCalendar/releases/tag/v1.0.0
