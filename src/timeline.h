// timeline.h — the strip: geometry, labels, and all the drawing.
//
//   [ left gutter ][8][            timeline            ][8][ right gutter ]
//                                ^ red now line, fixed at the centre
//
// Time flows right to left. The now line never moves; blocks drift leftward
// past it. There are no tick marks and no gridlines -- just past, now and
// future -- because the question the app answers is "where am I", not "what
// time is it".

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "ics.h"
#include "raii.h"

namespace rc {

// One run of text in a gutter label. Only the segment marked `truncatable` --
// the event name -- may be shortened. The countdown and the overlap badge are
// never cut, because a truncated countdown is worse than no countdown.
struct LabelSegment {
    std::wstring text;
    bool bold = false;
    bool truncatable = false;

    // Drawn in the alert red regardless of the rest of the label. Only the
    // simulated-clock marker uses this: a strip showing a time that is not the
    // time needs to say so in a way that cannot be mistaken for ordinary
    // content, and red is the one colour the rest of the label never uses.
    bool accent = false;
};

struct GutterLabel {
    std::vector<LabelSegment> segments;
    // Colour and weight diverge, which is why this is two flags rather than
    // one. `shouting` is true for the whole of the urgent window or the Ending
    // Soon Flash window and sets the weight, changing exactly once so the label
    // cannot jitter or resize as it blinks. `lit` is the blink phase and is
    // always true unless the flash is running.
    bool shouting = false;
    bool lit = true;
    int width = 0;            // measured, capped at maxLabelWidth
    bool empty() const { return segments.empty(); }
};

// Everything the strip needs for one frame. Rebuilt each tick; cheap because
// the labels are cached against (second, events generation, dark mode,
// settings fingerprint) and needed twice -- once to size the window, once to
// paint it.
struct Frame {
    Seconds now = 0;
    GutterLabel left;
    GutterLabel right;
    std::wstring error;       // when set, replaces all drawing
    bool dark = false;
};

class Timeline {
public:
    Timeline();
    ~Timeline();

    // Fonts follow the shell's menu font and the current DPI.
    void UpdateFonts(int dpi);
    int Dpi() const { return dpi_; }

    // Whether the window this paints into is layered with the chroma key set.
    // When it is, the background is filled with that key and disappears; when
    // it is not, the key would render as a near-black slab, so an approximation
    // of the taskbar's own colour is used instead.
    void SetTransparentBackground(bool transparent);

    void SetEvents(std::vector<CalEvent> events);
    void SetError(const std::wstring& message);   // empty clears it
    void InvalidateLabelCache();

    // Composes the gutters for `now` and reports the total width the widget
    // wants, in physical pixels. Callers apply a one-pixel dead-band before
    // resizing: a resize forces a taskbar relayout, so hysteresis matters.
    int Measure(Seconds now);

    // Paints into `dc` over `bounds`. Double-buffers internally; no GDI object
    // is created here that was not already cached.
    void Paint(HDC dc, const RECT& bounds);

    // What the tooltip says: the same thing the gutters say, in one line.
    std::wstring TooltipText(Seconds now) const;

    const std::vector<CalEvent>& Events() const { return events_; }

private:
    struct Impl;
    Impl* impl_;
    int dpi_ = 96;
    std::vector<CalEvent> events_;
};

namespace timeline {

// The colour the strip fills its background with, and which the window's
// layered attributes make transparent.
//
// Exposed so that the paint code and the window setup cannot disagree about
// which colour disappears. If they did, the result would be an opaque slab with
// nothing in either file looking wrong -- the kind of bug that takes a day.
COLORREF ChromaKey();

// Picks the event that gets to headline a gutter.
//
//   1. Time first: the left gutter takes whatever ends soonest, the right
//      whatever starts soonest.
//   2. Ties broken by a chain score -- +2 if another event's end meets this
//      one's start within a minute, +2 if another's start meets its end. This
//      favours the back-to-back time-blocked backbone over a meeting dropped
//      on top of it.
//   3. Then shorter duration, then title, purely so it cannot flicker.
//
// Only events with end > start are eligible; a "(0s)" label helps nobody.
const CalEvent* PickChained(const std::vector<CalEvent>& candidates,
                            bool byEndSoonest,
                            const std::vector<CalEvent>& all);

// How many events are running at `t`. Zero-length reminders count, treated as
// occupying a nominal minute.
int RunningCount(const std::vector<CalEvent>& events, Seconds t);

// How many upcoming events clash with `next`, plus the currently running block
// if it overlaps -- so a meeting dropped inside a block is flagged before it
// arrives. Once a clash crosses the now line the left gutter picks it up
// instead, which is what makes the badge's side mean "when".
int ClashCount(const std::vector<CalEvent>& events, const CalEvent& next, Seconds now);

}  // namespace timeline
}  // namespace rc
