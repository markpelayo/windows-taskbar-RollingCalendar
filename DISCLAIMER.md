# Disclaimer

Rolling Calendar for the Windows taskbar is provided free of charge, as is, and without warranty of any kind. There is no support, no service level and no undertaking that it will be maintained, fixed or updated.

**It is a convenience indicator, not a monitoring or scheduling tool.** It draws a picture of what a calendar feed said the last time it was successfully read. It is not a clock, not an alarm system, not a reminder service and not a source of truth about your day. Where a feed is stale, cached upstream, partially parsed or unreachable, the strip will be wrong, and in most of those cases it will be wrong quietly.

**It must not be relied on in any situation where being wrong could cause loss, harm or missed obligation.** That includes, and is not limited to, clinical settings, transport, safety-critical work, legal or regulatory deadlines, medication timing and anything with a financial consequence. Use a calendar application with alarms for those things.

The software may fail to display, fail to alert, alert at the wrong time, show events that have been cancelled, omit events that exist, or misplace events by a whole time zone. Every one of these is a known possible outcome of reading a third-party feed with a partial parser, and none of them is treated as an emergency by this project.

**No liability is accepted.** To the fullest extent permitted by law, the author accepts no liability for any loss or damage of any kind arising from the use of, or inability to use, this software. This includes appointments missed, work disrupted, data misread and time wasted. The full legal position is in [LICENSE](LICENSE); this paragraph is a summary of it, not a modification.

**It relies on undocumented behaviour of the Windows shell.** To place a strip inside the taskbar, the application re-parents a child window into the taskbar's own window. The individual API calls involved are documented and supported; the shell's willingness to host a foreign child window is not, and Microsoft has never committed to it. It may stop working at any time, in any Windows update, with no warning and no recourse. The application falls back to a floating window when embedding fails, but that fallback is a mitigation rather than a guarantee.

**As of version 1.0.0 the software has not been run on physical hardware by its author.** It compiles and it starts, which is a considerably weaker claim than working. Treat it accordingly.

If any of the above is not acceptable for your purposes, do not use the software.
