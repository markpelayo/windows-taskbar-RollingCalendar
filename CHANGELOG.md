# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
- A `Text Size` submenu: the shell's own menu-font size by default, so the strip matches everything else in the bar, plus 11, 13 and 15 pt presets and custom sizes from 6 to 48 pt. Custom sizes are remembered so they can be chosen again without retyping, and each has a Remove row, because a list you can add to but not take from fills up permanently.
- `Ending Soon Flash`, ported from the macOS version 1.6.3: for the last N minutes of the running block, its name in the left gutter blinks red once a second. Off by default, with presets of one, two and five minutes and a custom value from a quarter of a minute to sixty. The label's weight changes once at the start of the window rather than with the blink, so it cannot jitter or resize as it flashes.
- Reset Strip Settings covers the strip's geometry only — time range, timeline width, the label toggles, label length and text size. It deliberately leaves Ending Soon Flash alone; that is a warning, not a proportion, and a click aimed at the timeline's dimensions should not silently switch off the thing telling you a meeting is about to end. Restore Defaults clears it.
- All-day and zero-length events are excluded from the strip and shown only in the dropdown.

**Taskbar hosting**

- A child window re-parented into `Shell_TrayWnd`, so the strip moves, hides and auto-hides with the taskbar. Written from the public documentation for `FindWindow`, `SetParent`, `SetWindowPos`, `SetWindowLongPtr` and `SetLayeredWindowAttributes`. The re-parenting is confirmed with `GetParent` rather than trusted from `SetParent`'s return value, which is null both for failure and for a window that had no parent.
- The strip is a **layered** child (`WS_EX_LAYERED`). This is what makes it visible at all on Windows 11: the shell renders its taskbar through a composition island, and DWM draws a composited visual above the GDI painting of sibling windows regardless of the legacy child z-order, so an ordinary child sitting at the very front of the taskbar's children is still underneath the bar's own pixels. Being layered gets the window redirected into a composition visual of its own, above the island. `WS_EX_LAYERED` on a child window has been supported since Windows 8; a great many references, including an earlier comment in this project's own source, say otherwise, and that assumption cost a day.
- Real transparency, as a consequence of the above. A colour key on the layered window makes the strip's background see-through to the taskbar rather than an approximated slab. Guessing the bar's colour was never going to work: with transparency effects on it is acrylic over the user's wallpaper and there is no colour to guess. The key is defined once, in `timeline::ChromaKey()`, and read by both the window setup and the paint code, so the two cannot disagree.
- Automatic placement immediately left of the notification area, which is the only part of the taskbar that stays put, and to the left of any other application's widget already embedded there. Two apps using this technique would otherwise both claim the same spot and one would silently cover the other. When there is no clear gap it overlaps deliberately and says so in the log, because from the outside a deliberate overlap and a placement bug look identical.
- `Move widget…` and `Reset widget position`, storing the position as a distance from the taskbar's right edge.
- `Show on Display`, a picker over every taskbar the shell currently has. It only appears when there is more than one, since a menu offering a choice of one is a puzzle rather than a setting. The choice is stored as the adapter device name rather than an index, because indices are reassigned when a display is unplugged and a preference that quietly comes to mean a different screen after a dock is worse than one that fails to apply. A missing display falls back to the primary and leaves the preference intact.
- Re-embedding on the `TaskbarCreated` broadcast, so an Explorer restart does not permanently detach the widget, and re-assertion of the strip's place in the child order on every move and on the periodic poll, since the shell reorders its children whenever it relayouts.
- Periodic re-checking of the taskbar's position, edge and DPI, since the shell announces none of them. The edge is inferred from geometry, because `SHAppBarMessage(ABM_GETTASKBARPOS)` reports the primary bar only.
- A floating always-on-top fallback on every failure path — taskbar not found, `SetParent` failed, layering failed, shell replaced — so the widget is never invisible.
- A notification-area icon carrying the same menu and a tooltip with the same text as the gutters, as a guaranteed route to the app when the strip cannot be reached.
- `hostOverride` in the settings file to force the choice: plain child, layered child or floating. The failure it works around is a widget that cannot be seen, and the alternative to a one-line edit in an INI file would have been a rebuild.

**Diagnostics**

- An opt-in diagnostic log, off unless `diagnosticLog=1` is set under `[hidden]`. It writes `RollingCalendar-log.txt` beside the executable, falling back to `%APPDATA%\RollingCalendar` when that folder is not writable.
- It exists because of the Windows 11 composition island. Every diagnostic the app could take of its own window said the strip was correct — visible, sized, positioned, at the front of the taskbar's children — and it could not be seen. Finding that required a log of the shell's child z-order with our own window marked. The taskbar is somebody else's window and its internals are not contractual, so the next machine where the strip does not appear will need the same evidence, and "install a debugger" is not a reasonable thing to ask of anyone.
- Verbosity is high but bounded: everything at startup, a heartbeat every ten seconds, and a full snapshot whenever the menu is opened, which gives the user a way to mark the moment something looked wrong. A couple of kilobytes a minute, truncated on every launch.

**Calendar data**

- A hand-rolled RFC 5545 parser covering `FREQ=DAILY|WEEKLY|MONTHLY|YEARLY` with `INTERVAL`, `BYDAY`, `BYMONTHDAY`, `UNTIL` and `COUNT`, plus `EXDATE` and `RECURRENCE-ID` overrides. Recurrence is expanded per requested day; non-recurring events are filtered to the four loaded days as they are parsed.
- Link normalisation accepting Google Calendar embed links, "add by URL" links, `webcal://` links, any URL ending `.ics`, a bare calendar address, and `file://` paths to local `.ics` files.
- Display time zone taken from a link's `ctz=` parameter where present, resolved through a CLDR-derived IANA-to-Windows mapping table, falling back to the machine's zone on a miss.
- Feed fetching over WinHTTP with a 20-second timeout and the local cache bypassed, every five minutes, at launch, on wake from sleep and on any source change. Stale responses are dropped by generation token.
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
- An anchor-to-anchor day list, capped at 60 rows with date separators, aligned by computed tab stops rather than by padding, showing a marker and bold title for the block in progress, plus duration, category swatch and overlap badge per row.
- An ISO 8601 week-number caption and a freshness caption measured on the real clock rather than the simulated one.
- Debug Time: an offset rather than a freeze, so the simulated clock keeps running and countdowns still tick. While it is active the strip carries a red marker, because a strip showing a time that is not the time has to say so in a way that cannot be mistaken for ordinary content.

**Sound**

- Time Block Alerts with lead times as a set rather than a single choice, a category filter, mutually exclusive sound and speech output, SAPI 5 voices, sounds from `%WINDIR%\Media` plus imports, and a lateness rule that marks an alert more than thirty seconds late as fired and skips it silently. Announcing "ten minutes before" when there are three minutes left is not a late alert, it is a wrong one.
- The Westminster Quarters synthesised from inharmonic sine partials, with an optional hour strike and adjustable volume. Nothing is bundled and nothing is downloaded: a recording of Big Ben is copyright, the 1793 tune is not. Rung quarters are recorded by absolute index rather than wall-clock time, so a daylight-saving fall-back does not silence the repeated hour.
- A single Sound Hours schedule gating both features, taking a set of windows, with inclusive endpoints, midnight wrapping and an explicit all-day option.

**Housekeeping**

- Settings in one INI file at `%APPDATA%\RollingCalendar\settings.ini`, cached in memory and written through on change. Restore Defaults deletes the whole file rather than a list of keys, so a setting added in a later version cannot be left behind by an older reset.
- Hidden settings under `[hidden]` for the now-line width, urgent threshold, unmatched colour, solid or translucent blocks, block gap, corner radius, day anchor keyword, host mode override, diagnostic log, past-block fade, capsule height and the gap between a gutter and the timeline. `titleFontSize` is *not* among them: it moved to `[strip]` when it gained a menu item.
- Run at Startup through a Task Scheduler logon task with a native delay of 5 to 60 seconds, rather than a registry Run entry, which cannot express one. Signing in is the busiest moment the machine will have all day and a calendar widget has no business competing for it. The task is rewritten if the executable has moved.
- One second timer driving the redraw, the re-measure, the alert check and the chime check. Hysteretic resizing so the taskbar does not relayout for a single pixel.
- Every GDI handle owned by an RAII wrapper, fonts and colours cached per DPI, no child processes and no iostreams.
- `build.bat` for a single MSVC invocation and `CMakeLists.txt` for editors and CI, producing the same statically linked binary. No vcpkg, no NuGet, no submodules and no third-party libraries.
- A GitHub Actions workflow building both paths, checking the binary launches and stays up, and attaching a zip to tagged releases.

### What has and has not been verified

Verified in use on **Windows 11 24H2, build 26100**, on one machine: at 3440x1440 and at 1920x1080, on a bottom-docked taskbar at 100% scaling, in both the light and dark system themes, and alongside the author's [windows-taskbar-pinger](https://github.com/markpelayo/windows-taskbar-pinger) embedded in the same taskbar. The strip embeds, draws, positions itself around the other widget, survives an Explorer restart and reads a real Google Calendar feed.

Not verified at all: **Windows 10 of any build, fractional display scaling, taskbars docked to the left or right edge, auto-hide taskbars, more than one taskbar, and ARM64.** Those are the configurations where a widget that positions itself by measuring the taskbar is most likely to be wrong, and reports from them — with `diagnosticLog=1` — are the most useful thing anyone could send.

Separately from anything testing can settle: **the taskbar embedding relies on undocumented shell behaviour.** Every call it makes is documented and supported, but nothing in the documentation says the taskbar will tolerate a foreign child window living inside it, and Microsoft has never committed to it. That behaviour already changed once, between the Windows 10 taskbar and the Windows 11 composition island, and the change was invisible until it was investigated. It works here; it is not promised to keep working anywhere.

[1.0.0]: https://github.com/markpelayo/windows-taskbar-RollingCalendar/releases/tag/v1.0.0
