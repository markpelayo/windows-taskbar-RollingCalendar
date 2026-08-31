// timeline.cpp — see timeline.h for what the strip is and why it looks the way
// it does. This file is the whole of the drawing.
//
// Two constraints shape it.
//
// The first is that a repaint must allocate nothing. The strip redraws once a
// second for as long as the machine is switched on, and a process is capped at
// 10,000 GDI handles, so one leaked brush per frame kills the app inside three
// hours. Every brush, pen, font and bitmap lives in Impl, wrapped in an RAII
// type from raii.h, and is rebuilt only when a colour, the DPI or the widget's
// size actually changes.
//
// The second is that GDI has no alpha. The macOS original leans on translucent
// fills for the halo behind the now line, for the block outlines and for the
// non-solid block style. Each of those is reproduced here by blending the two
// colours arithmetically -- against the taskbar background where there is
// nothing better to blend against -- which is exact wherever the thing
// underneath really is the background and a close approximation where it is
// not. The alternative is a second buffer and an AlphaBlend per block, which
// costs more than the difference is worth.
//
// Because the header fixes the public surface, every internal step lives on
// Impl rather than on Timeline; the public methods are thin forwarders.

#include "timeline.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <unordered_map>

#include "settings.h"
#include "taskbar.h"

namespace rc {
namespace {

// #FF3B30, the systemRed the original uses for the now line, the urgent label
// and the overlap badge. Named because it turns up in four places.
constexpr COLORREF kSystemRed = RGB(0xFF, 0x3B, 0x30);

// The overlap badge marker. macOS draws a red emoji here; a filled circle drawn
// with the same brush as the now line renders identically at every DPI and does
// not depend on an emoji font being installed, which on a Windows N edition or
// a locked-down corporate image is not a safe assumption. The dot is carried
// through the label as a private-use sentinel at the head of the badge segment,
// so LabelSegment stays a plain run of text and the header does not have to
// grow a flag for it.
constexpr wchar_t kBadgeDot = static_cast<wchar_t>(0xE000);

// The colour made transparent by the window's layered attributes. Kept here and
// exposed through ChromaKey() so app.cpp cannot disagree with the paint code
// about which colour disappears -- a mismatch there would show as an opaque
// slab, with nothing in either file looking wrong.
constexpr COLORREF kChromaKey = RGB(0x01, 0x02, 0x03);

// How far the elapsed part of a block is blended toward white. The macOS
// original uses 0.45 dark and 0.60 light; both are pushed further here because
// the strip now sits on the taskbar itself rather than on a flat fill, and a
// difference that read clearly against black does not against a photograph.
constexpr double kPastFadeDark = 0.58;
constexpr double kPastFadeLight = 0.68;

// How far a block's outline is blended toward black. Also softened: a hard
// black keyline looked like a border against a flat background and looks like
// grime against a translucent one.
constexpr double kOutlinePast = 0.18;
constexpr double kOutlineFuture = 0.32;

// Logical px between a gutter and the strip. Enough to stop the text reading as
// attached to whichever capsule it is touching, and no more: every pixel here
// is charged twice, once on each side, and taken off the taskbar.
constexpr int kInnerGap = 6;

// Tunable, because how much separation reads as separation depends on the
// wallpaper the capsules are now sitting on.
double InnerGap() { return Cfg().innerGap > 0 ? Cfg().innerGap : kInnerGap; }

constexpr int kTrackInset = 2;       // minimum vertical clearance above and below blocks

// Height of the capsule band in logical px, chosen to match a taskbar icon.
constexpr int kBlockHeight = 24;
constexpr int kBlockOvershoot = 12;  // how far a long block may run past the strip
constexpr int kMinBlockWidth = 3;
constexpr int kMinAvailable = 12;    // below this a name is dropped, not truncated
constexpr int kChainTolerance = 60;  // seconds; what counts as one block "meeting" another

int Scale(double logical, int dpi) {
    return static_cast<int>(std::lround(logical * static_cast<double>(dpi) / 96.0));
}

LONG RectWidth(const RECT& r) { return r.right - r.left; }
LONG RectHeight(const RECT& r) { return r.bottom - r.top; }

long long AbsDiff(Seconds a, Seconds b) {
    return std::llabs(static_cast<long long>(a) - static_cast<long long>(b));
}

// Zero-length reminders occupy a nominal minute wherever an overlap is counted.
// Without it they could not overlap anything and would never be flagged, which
// is the opposite of what a reminder is for.
Seconds NominalEnd(const CalEvent& e) {
    return (e.end > e.start + 60) ? e.end : e.start + 60;
}

// Identity by value. The candidate lists are copies, so pointer comparison
// cannot recognise an event as itself when scanning the full list.
bool SameEvent(const CalEvent& a, const CalEvent& b) {
    return a.start == b.start && a.end == b.end && a.title == b.title;
}

// Exactly the values a label reads. Anything absent from here cannot change a
// label; anything present must invalidate the cache when it moves.
uint64_t SettingsFingerprint() {
    const Settings& c = Cfg();
    uint64_t h = 1469598103934665603ull;
    const auto mixBits = [&h](uint64_t bits) {
        h ^= bits;
        h *= 1099511628211ull;
    };
    const auto mixDouble = [&mixBits](double d) {
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        mixBits(bits);
    };
    const auto mixBool = [&mixBits](bool b) { mixBits(b ? 1ull : 2ull); };

    mixDouble(c.maxLabelWidth);
    mixDouble(c.titleFontSize);
    mixBool(c.showNowName);
    mixBool(c.showNowTimeLeft);
    mixBool(c.showNextName);
    mixBool(c.showNextDuration);
    mixBool(Clock::IsSimulating());
    mixDouble(c.urgentSeconds);
    return h;
}

// The taskbar carries its own theme preference, separate from the apps theme,
// and the strip sits inside the taskbar. Asking IsDarkMode() here would follow
// the wrong switch and put dark text on a dark bar for anyone running the very
// common "light apps, dark system" combination.
bool TaskbarIsDark() {
    const COLORREF text = TaskbarTextColor();
    return (GetRValue(text) + GetGValue(text) + GetBValue(text)) > 3 * 128;
}

std::wstring Badge(int n) {
    std::wstring s(1, kBadgeDot);
    s += Format(L"(%d)", n);
    return s;
}

}  // namespace

// ----------------------------------------------------------------------------

struct Timeline::Impl {
    // ---- the GDI cache --------------------------------------------------
    //
    // Nothing below is created during a repaint. Each entry is rebuilt only
    // when its colour, the DPI or the widget's size changes; the colour-keyed
    // maps fill on first sight of a calendar colour and are then reused for the
    // life of the process. A process is capped at 10,000 GDI handles, so a
    // handle created per frame is not a leak anyone notices in testing, it is a
    // crash on a timer -- about three hours at one repaint a second. Every
    // handle here is owned by an RAII wrapper from raii.h.
    UniqueFont normalFont;
    UniqueFont boldFont;
    UniqueMemDC measureDc;

    UniqueMemDC backDc;
    UniqueBitmap backBitmap;
    HGDIOBJ backOldBitmap = nullptr;
    LONG backWidth = 0;
    LONG backHeight = 0;

    UniqueBrush bgBrush;
    COLORREF bgColor = CLR_INVALID;
    UniqueBrush redBrush;
    UniqueBrush haloBrush;
    COLORREF haloColor = CLR_INVALID;

    std::unordered_map<COLORREF, UniqueBrush> blockBrushes;
    std::unordered_map<COLORREF, UniquePen> outlinePens;

    // ---- metrics, recomputed with the fonts -----------------------------
    int dpi = 96;
    int textHeight = 12;
    int dotDiameter = 7;
    int dotAdvance = 9;

    // ---- state ----------------------------------------------------------
    std::wstring error;
    bool transparent = false;
    uint64_t generation = 0;
    bool haveNow = false;
    Seconds lastNow = 0;

    // ---- label cache ----------------------------------------------------
    //
    // Keyed on the integer second, the events generation, the taskbar theme and
    // the settings fingerprint, because the labels are wanted twice per tick:
    // once to work out how wide the window should be, and again to paint it.
    // Composing them twice would run the truncation fitting loop twice, and
    // that loop is the only part of a frame that measures text repeatedly.
    bool cacheValid = false;
    Seconds cacheSecond = 0;
    uint64_t cacheGeneration = 0;
    bool cacheDark = false;
    uint64_t cacheFingerprint = 0;
    Frame frame;

    // Reused across frames so a repaint does not touch the heap either.
    std::vector<const CalEvent*> drawOrder;
    std::vector<CalEvent> pickBuffer;

    ~Impl() {
        if (backDc && backOldBitmap) ::SelectObject(backDc.get(), backOldBitmap);
    }

    // ---- cached handles --------------------------------------------------

    HBRUSH BrushFor(COLORREF c) {
        auto it = blockBrushes.find(c);
        if (it == blockBrushes.end()) {
            it = blockBrushes.emplace(c, UniqueBrush(::CreateSolidBrush(c))).first;
        }
        return it->second.get();
    }

    HPEN PenFor(COLORREF c) {
        auto it = outlinePens.find(c);
        if (it == outlinePens.end()) {
            it = outlinePens.emplace(c, UniquePen(::CreatePen(PS_SOLID, 1, c))).first;
        }
        return it->second.get();
    }

    void SetBackground(COLORREF c) {
        if (bgColor == c && bgBrush) return;
        bgBrush.reset(::CreateSolidBrush(c));
        bgColor = c;
    }

    void SetHalo(COLORREF c) {
        if (haloColor == c && haloBrush) return;
        haloBrush.reset(::CreateSolidBrush(c));
        haloColor = c;
    }

    // ---- measurement ------------------------------------------------------

    int TextWidth(const std::wstring& text, bool bold) {
        if (text.empty()) return 0;
        const HDC dc = measureDc.get();
        if (!dc) return 0;
        SelectGuard sel(dc, bold ? boldFont.get() : normalFont.get());
        SIZE sz{};
        if (!::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz)) {
            return 0;
        }
        // The original returns ceil(measured) + 1. The spare pixel stops a name
        // that measured exactly to the cap from being clipped by rounding.
        return sz.cx + 1;
    }

    int SegmentWidth(const LabelSegment& seg, bool bold) {
        if (!seg.text.empty() && seg.text[0] == kBadgeDot) {
            return dotAdvance + TextWidth(seg.text.substr(1), bold);
        }
        return TextWidth(seg.text, bold);
    }

    // Shrink-then-grow, exactly as spec 3.7 describes. The proportional guess
    // lands within a character or two, so the loops that follow it rarely run
    // more than a couple of iterations; walking in from either end instead
    // would measure the whole name dozens of times per tick.
    std::wstring Fitted(const std::wstring& text, bool bold, int available) {
        if (available <= Scale(kMinAvailable, dpi)) return std::wstring();

        const int full = TextWidth(text, bold);
        if (full <= available) return text;
        if (text.empty() || full <= 0) return std::wstring();

        const int count = static_cast<int>(text.size());
        int keep = static_cast<int>(static_cast<double>(count) *
                                   (static_cast<double>(available) / static_cast<double>(full)));
        keep = std::max(0, std::min(keep, count));

        const auto withEllipsis = [&text](int k) {
            return text.substr(0, static_cast<size_t>(k)) + L"\x2026";
        };

        while (keep > 0 && TextWidth(withEllipsis(keep), bold) > available) --keep;
        while (keep < count && TextWidth(withEllipsis(keep + 1), bold) <= available) ++keep;

        if (keep <= 0) return std::wstring();
        return withEllipsis(keep);
    }

    // Truncates the one truncatable segment to fit, then measures the label.
    // Fixed segments are measured in the weight they will be drawn in: bold is
    // wider, and a label sized in the wrong weight is the one that overflows.
    void FinishLabel(GutterLabel& label, int cap) {
        label.width = 0;
        if (label.segments.empty()) return;

        const bool urgent = label.urgent;
        const auto boldOf = [urgent](const LabelSegment& s) { return s.bold || urgent; };

        size_t nameIndex = label.segments.size();
        for (size_t i = 0; i < label.segments.size(); ++i) {
            if (label.segments[i].truncatable) {
                nameIndex = i;
                break;
            }
        }

        if (nameIndex < label.segments.size()) {
            const int count = static_cast<int>(label.segments.size());
            const int spacing =
                (count > 1)
                    ? TextWidth(std::wstring(static_cast<size_t>(count - 1), L' '), urgent)
                    : 0;

            int fixed = 0;
            for (size_t i = 0; i < label.segments.size(); ++i) {
                if (i == nameIndex) continue;
                fixed += SegmentWidth(label.segments[i], boldOf(label.segments[i]));
            }

            LabelSegment& name = label.segments[nameIndex];
            name.text = Fitted(name.text, boldOf(name), cap - fixed - spacing);
            if (name.text.empty()) {
                // Dropping the name changes the segment count and therefore the
                // spacing, so the total below is worked out afresh either way.
                label.segments.erase(label.segments.begin() +
                                     static_cast<std::ptrdiff_t>(nameIndex));
            }
        }

        if (label.segments.empty()) return;

        const int count = static_cast<int>(label.segments.size());
        int total = 0;
        for (const auto& seg : label.segments) total += SegmentWidth(seg, boldOf(seg));
        if (count > 1) {
            total += TextWidth(std::wstring(static_cast<size_t>(count - 1), L' '), urgent);
        }
        label.width = std::min(total + 1, cap);
    }

    // ---- composition ------------------------------------------------------

    void ComposeGutters(const std::vector<CalEvent>& events, Seconds now) {
        const Settings& cfg = Cfg();
        const int cap = std::max(1, Scale(cfg.maxLabelWidth, dpi));

        // All-day blocks are filtered out first and stay out: they feed neither
        // gutter nor its badge, because "all day" is not an answer to the
        // question "what am I meant to be doing".
        pickBuffer.clear();
        for (const auto& e : events) {
            if (!e.isAllDay && e.end > e.start && e.runningAt(now)) pickBuffer.push_back(e);
        }
        const CalEvent* running = timeline::PickChained(pickBuffer, true, events);
        const bool haveCurrent = running != nullptr;
        const CalEvent current = haveCurrent ? *running : CalEvent();

        GutterLabel& left = frame.left;
        left = GutterLabel();
        const double remaining = haveCurrent ? static_cast<double>(current.end - now) : 0.0;
        left.urgent = haveCurrent && remaining <= cfg.urgentSeconds;

        if (Clock::IsSimulating()) {
            // Red, bold, and flanked by exclamation marks. It has to be
            // impossible to mistake for content: every time shown beside it is
            // a lie, and someone who has forgotten they left Debug Time on will
            // otherwise trust the strip and miss a meeting.
            left.segments.push_back({L"!Simulated!", true, false, true});
        }
        const int runningNow = timeline::RunningCount(events, now);
        if (runningNow > 1) left.segments.push_back({Badge(runningNow), false, false, false});
        if (haveCurrent && cfg.showNowName) {
            left.segments.push_back({current.title, false, true, false});
        }
        if (haveCurrent && cfg.showNowTimeLeft) {
            left.segments.push_back({L"(" + FormatDuration(remaining) + L")", false, false, false});
        }
        FinishLabel(left, cap);

        pickBuffer.clear();
        for (const auto& e : events) {
            if (!e.isAllDay && e.end > e.start && e.start >= now) pickBuffer.push_back(e);
        }
        const CalEvent* upcoming = timeline::PickChained(pickBuffer, false, events);

        GutterLabel& right = frame.right;
        right = GutterLabel();  // never bold, never red: it has not happened yet
        if (upcoming) {
            const CalEvent next = *upcoming;
            if (cfg.showNextDuration) {
                right.segments.push_back(
                    {L"(" + FormatDuration(next.duration()) + L")", false, false});
            }
            if (cfg.showNextName) right.segments.push_back({next.title, false, true, false});
            const int clashes = timeline::ClashCount(events, next, now);
            if (clashes > 1) right.segments.push_back({Badge(clashes), false, false, false});
        }
        FinishLabel(right, cap);
    }

    void EnsureLabels(const std::vector<CalEvent>& events, Seconds now) {
        const bool dark = TaskbarIsDark();
        const uint64_t fingerprint = SettingsFingerprint();

        if (cacheValid && cacheSecond == now && cacheGeneration == generation &&
            cacheDark == dark && cacheFingerprint == fingerprint) {
            return;
        }

        frame.now = now;
        frame.dark = dark;
        frame.error = error;
        ComposeGutters(events, now);

        cacheValid = true;
        cacheSecond = now;
        cacheGeneration = generation;
        cacheDark = dark;
        cacheFingerprint = fingerprint;
    }

    // ---- drawing ----------------------------------------------------------

    // `area` is the gutter box. The left gutter is right-aligned against it and
    // the right gutter left-aligned, so both labels sit hard against the strip
    // and grow outward as their text lengthens.
    void DrawLabel(HDC dc, const GutterLabel& label, const RECT& area, bool rightAligned,
                   COLORREF colour) {
        if (label.segments.empty()) return;

        const bool urgent = label.urgent;
        const auto boldOf = [urgent](const LabelSegment& s) { return s.bold || urgent; };

        const int count = static_cast<int>(label.segments.size());
        const int spaceWidth = TextWidth(L" ", urgent);

        int total = 0;
        for (const auto& seg : label.segments) total += SegmentWidth(seg, boldOf(seg));
        total += spaceWidth * (count - 1);

        int x = rightAligned ? static_cast<int>(area.right) - total : static_cast<int>(area.left);
        const int y = static_cast<int>(area.top) +
                      (static_cast<int>(RectHeight(area)) - textHeight) / 2;

        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, colour);

        for (int i = 0; i < count; ++i) {
            const LabelSegment& seg = label.segments[static_cast<size_t>(i)];
            std::wstring text = seg.text;

            if (!text.empty() && text[0] == kBadgeDot) {
                const int d = dotDiameter;
                const int cy = static_cast<int>(area.top) + static_cast<int>(RectHeight(area)) / 2;
                SelectGuard brush(dc, redBrush.get());
                SelectGuard pen(dc, ::GetStockObject(NULL_PEN));
                ::Ellipse(dc, x, cy - d / 2, x + d, cy - d / 2 + d);
                x += dotAdvance;
                text.erase(text.begin());
            }

            if (!text.empty()) {
                SelectGuard font(dc, boldOf(seg) ? boldFont.get() : normalFont.get());
                // An accented segment keeps its own colour whatever the label
                // around it is doing, and hands it back afterwards so the next
                // segment is unaffected.
                if (seg.accent) ::SetTextColor(dc, kSystemRed);
                ::TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
                if (seg.accent) ::SetTextColor(dc, colour);
                x += TextWidth(text, boldOf(seg));
            }

            if (i + 1 < count) x += spaceWidth;
        }
    }

    void DrawBlocks(HDC dc, const std::vector<CalEvent>& events, const RECT& strip,
                    const RECT& track, Seconds now, bool dark, COLORREF background) {
        const Settings& cfg = Cfg();

        const double stripWidth = static_cast<double>(RectWidth(strip));
        if (stripWidth <= 0.0) return;

        // Clip everything to the strip.
        //
        // A block longer than the visible window is drawn overshooting both
        // ends, so that its rounded cap falls outside the strip and the edge
        // reads as flat -- which is the point: a curved end means the block
        // starts or finishes there, and a block still running when it leaves
        // the window has not finished. Without this clip the left-hand cap
        // landed inside the widget and drew a rounded end, while the right-hand
        // one fell off the back buffer and was cut flat. Rounded at one end,
        // square at the other, saying two different things about the same
        // block.
        DcStateGuard clipState(dc);
        ::IntersectClipRect(dc, strip.left, strip.top, strip.right, strip.bottom);

        const double windowSeconds = std::max(1.0, cfg.windowMinutes * 60.0);
        const Seconds half = static_cast<Seconds>(windowSeconds / 2.0);
        const Seconds windowStart = now - half;
        const Seconds windowEnd = now + half;

        const double pxPerSec = stripWidth / windowSeconds;
        const double nowX = (strip.left + strip.right) / 2.0;
        const auto X = [&](Seconds t) {
            return nowX + static_cast<double>(t - now) * pxPerSec;
        };

        drawOrder.clear();
        for (const auto& e : events) {
            // All-day blocks would swamp the strip and zero-length reminders
            // would be a stray sliver. Both still count towards the badges.
            if (e.isAllDay || e.end <= e.start) continue;
            if (!e.intersects(windowStart, windowEnd)) continue;
            drawOrder.push_back(&e);
        }

        // Longest first, so the shortest concurrent block ends up on top and
        // stays visible. The strip is one row: overlaps are flagged, never
        // stacked.
        std::stable_sort(drawOrder.begin(), drawOrder.end(),
                         [](const CalEvent* a, const CalEvent* b) {
                             if (a->duration() != b->duration()) return a->duration() > b->duration();
                             return a->start < b->start;
                         });

        const double gapPx = static_cast<double>(Scale(cfg.blockGap, dpi));
        const double cornerPx = static_cast<double>(Scale(cfg.blockCornerRadius, dpi));
        const LONG overshoot = Scale(kBlockOvershoot, dpi);
        const int minWidth = std::max(1, Scale(kMinBlockWidth, dpi));

        for (const CalEvent* e : drawOrder) {
            const double x0 = std::max(X(e->start), static_cast<double>(strip.left - overshoot));
            const double x1 = std::min(X(e->end), static_cast<double>(strip.right + overshoot));
            const double fullWidth = std::max(x1 - x0, static_cast<double>(minWidth));

            // Cosmetic only. The trim narrows the drawn capsule so adjacent
            // blocks read as separate; it never alters the span they cover.
            const double trim = std::min(gapPx / 2.0, fullWidth * 0.2);

            RECT r{};
            r.left = static_cast<LONG>(std::lround(x0 + trim));
            r.top = track.top;
            r.bottom = track.bottom;
            r.right = r.left + std::max(static_cast<LONG>(std::lround(fullWidth - 2.0 * trim)),
                                        static_cast<LONG>(minWidth));

            const double halfMin = std::min(RectHeight(r), RectWidth(r)) / 2.0;
            const double radius = (cornerPx > 0.0) ? std::min(cornerPx, halfMin) : halfMin;
            const int diameter = std::max(0, static_cast<int>(std::lround(radius * 2.0)));

            const COLORREF base = e->color.value_or(cfg.unmatchedColor);
            const double fade = (Cfg().pastFade > 0.0)
                                    ? Cfg().pastFade
                                    : (dark ? kPastFadeDark : kPastFadeLight);
            const COLORREF pastBase = Highlight(base, fade);

            COLORREF futureFill = base;
            COLORREF pastFill = pastBase;
            if (!cfg.solidBlocks) {
                // GDI cannot fill a RoundRect through an alpha channel, so the
                // translucent style is emulated by blending the tint against
                // the taskbar background at the spec's alpha. Exact wherever
                // what lies underneath is the background, which on the strip it
                // almost always is.
                futureFill = Blend(background, base, dark ? 0.32 : 0.24);
                pastFill = Blend(background, pastBase, dark ? 0.18 : 0.14);
            }

            const LONG split = std::max(
                r.left, std::min(static_cast<LONG>(std::lround(nowX)), r.right));

            {
                SelectGuard pen(dc, ::GetStockObject(NULL_PEN));
                SelectGuard brush(dc, BrushFor(futureFill));
                ::RoundRect(dc, r.left, r.top, r.right, r.bottom, diameter, diameter);

                if (split > r.left) {
                    // A block in progress fades from the left as it elapses.
                    DcStateGuard state(dc);
                    ::IntersectClipRect(dc, r.left, r.top, split, r.bottom);
                    SelectGuard pastBrush(dc, BrushFor(pastFill));
                    ::RoundRect(dc, r.left, r.top, r.right, r.bottom, diameter, diameter);
                }
            }

            // Outline. GDI strokes a one-pixel pen on the inside edge of the
            // path, so passing the block rect unchanged reproduces the
            // original's insetBy(0.5, 0.5); insetting again would leave a ring
            // of unstroked fill outside the line. The two halves are stroked
            // under their own clips, expanded outward by a pixel so the
            // horizontal runs are not sliced lengthwise, but exact at the seam
            // so the darker future stroke cannot bleed over the past side.
            SelectGuard hollow(dc, ::GetStockObject(NULL_BRUSH));
            if (split > r.left) {
                DcStateGuard state(dc);
                ::IntersectClipRect(dc, r.left - 1, r.top - 1, split, r.bottom + 1);
                SelectGuard pen(dc, PenFor(Blend(pastFill, RGB(0, 0, 0), kOutlinePast)));
                ::RoundRect(dc, r.left, r.top, r.right, r.bottom, diameter, diameter);
            }
            if (split < r.right) {
                DcStateGuard state(dc);
                ::IntersectClipRect(dc, split, r.top - 1, r.right + 1, r.bottom + 1);
                SelectGuard pen(dc, PenFor(Blend(futureFill, RGB(0, 0, 0), kOutlineFuture)));
                ::RoundRect(dc, r.left, r.top, r.right, r.bottom, diameter, diameter);
            }
        }
    }

    void DrawNowLine(HDC dc, const RECT& strip, const RECT& track, bool dark,
                     COLORREF background) {
        const LONG lineWidth = std::max(1, Scale(Cfg().nowLineWidth, dpi));
        const LONG centre = static_cast<LONG>(std::lround((strip.left + strip.right) / 2.0));

        // The line is measured against the capsule band, not the whole strip,
        // with a small overhang at each end. Running it the full height of the
        // widget made it a divider through the gutters; overhanging the band
        // slightly keeps it reading as a marker on the timeline.
        const LONG overhang = std::max<LONG>(1, Scale(3, dpi));
        const LONG top = std::max(strip.top, track.top - overhang);
        const LONG bottom = std::min(strip.bottom, track.bottom + overhang);

        DcStateGuard state(dc);
        ::IntersectClipRect(dc, strip.left, strip.top, strip.right, strip.bottom);

        // The halo exists because pure red disappears against a warm-toned
        // capsule. Its 50% alpha is emulated by blending against the taskbar
        // background -- see the note at the top of this file about GDI.
        SetHalo(Blend(background, dark ? RGB(0, 0, 0) : RGB(255, 255, 255), 0.5));
        RECT halo{centre - lineWidth / 2 - 1, top, centre - lineWidth / 2 - 1 + lineWidth + 2,
                  bottom};
        ::FillRect(dc, &halo, haloBrush.get());

        RECT line{centre - lineWidth / 2, top, centre - lineWidth / 2 + lineWidth, bottom};
        ::FillRect(dc, &line, redBrush.get());
    }
};

// ----------------------------------------------------------------------------

Timeline::Timeline() : impl_(new Impl) {
    impl_->measureDc.reset(::CreateCompatibleDC(nullptr));
    impl_->redBrush.reset(::CreateSolidBrush(kSystemRed));
    UpdateFonts(96);
}

Timeline::~Timeline() {
    delete impl_;
}

void Timeline::UpdateFonts(int dpi) {
    dpi_ = (dpi > 0) ? dpi : 96;
    Impl& im = *impl_;
    im.dpi = dpi_;

    LOGFONTW lf{};
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMenuFont;
    } else {
        // A machine that will not report its non-client metrics is broken in a
        // way this app cannot fix, but it can still draw something legible.
        lf.lfHeight = -12;
        lf.lfCharSet = DEFAULT_CHARSET;
        ::wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    }

    if (Cfg().titleFontSize > 0) {
        const int points = static_cast<int>(std::lround(Cfg().titleFontSize));
        lf.lfHeight = -::MulDiv(points, dpi_, 72);
    } else {
        // SystemParametersInfoW reports at the system DPI, which for a
        // per-monitor-aware process is 96 whatever monitor the taskbar is on,
        // so the shell's own size has to be scaled by hand.
        lf.lfHeight = ::MulDiv(lf.lfHeight, dpi_, 96);
    }
    lf.lfQuality = CLEARTYPE_QUALITY;

    lf.lfWeight = FW_NORMAL;
    im.normalFont.reset(::CreateFontIndirectW(&lf));
    lf.lfWeight = FW_BOLD;
    im.boldFont.reset(::CreateFontIndirectW(&lf));

    if (im.measureDc && im.normalFont) {
        SelectGuard sel(im.measureDc.get(), im.normalFont.get());
        TEXTMETRICW tm{};
        if (::GetTextMetricsW(im.measureDc.get(), &tm)) im.textHeight = tm.tmHeight;
    }

    // The badge dot is sized off the text so it reads as punctuation rather
    // than as a bullet, and scales with everything else.
    im.dotDiameter = std::max(4, im.textHeight / 2);
    im.dotAdvance = im.dotDiameter + Scale(3, dpi_);

    im.cacheValid = false;
}

void Timeline::SetTransparentBackground(bool transparent) {
    if (impl_->transparent == transparent) return;
    impl_->transparent = transparent;
    // Drop the cached brush so the next paint builds one for the new colour.
    // SetBackground short-circuits on an unchanged colour, and the colour has
    // not changed -- only what it is going to be.
    impl_->bgBrush.reset();
}

void Timeline::SetEvents(std::vector<CalEvent> events) {
    events_ = std::move(events);
    impl_->generation++;
    impl_->cacheValid = false;
}

void Timeline::SetError(const std::wstring& message) {
    if (impl_->error == message) return;
    impl_->error = message;
    impl_->cacheValid = false;
}

void Timeline::InvalidateLabelCache() {
    impl_->cacheValid = false;
}

int Timeline::Measure(Seconds now) {
    Impl& im = *impl_;
    im.haveNow = true;
    im.lastNow = now;

    const int stripWidth = std::max(1, Scale(Cfg().timelineWidth, dpi_));

    if (!im.error.empty()) {
        // The error replaces everything else on the strip, so the widget only
        // has to be wide enough to read it -- but never narrower than the strip
        // it replaced, or the taskbar would relayout every time a fetch failed.
        return std::max(stripWidth, im.TextWidth(im.error, false) + Scale(2 * InnerGap(), dpi_));
    }

    im.EnsureLabels(events_, now);

    const int gap = Scale(InnerGap(), dpi_);
    int total = stripWidth;
    if (im.frame.left.width > 0) total += im.frame.left.width + gap;
    if (im.frame.right.width > 0) total += im.frame.right.width + gap;
    return total;
}

void Timeline::Paint(HDC dc, const RECT& bounds) {
    Impl& im = *impl_;

    const LONG width = RectWidth(bounds);
    const LONG height = RectHeight(bounds);
    if (width <= 0 || height <= 0) return;

    // Double buffer, rebuilt only when the widget actually changes size. A DPI
    // change is covered by the same test, because it always changes the size.
    if (!im.backDc) im.backDc.reset(::CreateCompatibleDC(dc));
    if (!im.backDc) return;
    if (!im.backBitmap || im.backWidth != width || im.backHeight != height) {
        if (im.backOldBitmap) {
            ::SelectObject(im.backDc.get(), im.backOldBitmap);
            im.backOldBitmap = nullptr;
        }
        im.backBitmap.reset(::CreateCompatibleBitmap(dc, width, height));
        if (!im.backBitmap) return;
        im.backOldBitmap = ::SelectObject(im.backDc.get(), im.backBitmap.get());
        im.backWidth = width;
        im.backHeight = height;
    }

    const HDC back = im.backDc.get();
    const Seconds now = im.haveNow ? im.lastNow : Clock::Now();
    const bool dark = TaskbarIsDark();

    // The background is filled with a chroma key rather than an approximation
    // of the taskbar's colour, and the window's layered attributes make that
    // colour transparent. So the capsules and the labels sit directly on the
    // bar, whatever the bar happens to look like -- which is the only approach
    // that survives a user's wallpaper showing through acrylic, since there is
    // no colour that could have been guessed for that.
    //
    // The key is a near-black that the drawing cannot produce by accident. Pure
    // black would be the obvious choice and is the wrong one: the outline
    // strokes and the now-line halo are blends toward black, so any of them
    // landing exactly on it would punch a hole through the strip. Three
    // separate off-by-one channels cannot arise from those blends.
    //
    // Antialiased glyph edges blend toward the key and therefore stay opaque,
    // which leaves a faint dark fringe around the text. That is a fair trade:
    // it reads as a soft shadow, and it is what keeps light text legible
    // against a light wallpaper.
    const COLORREF background =
        im.transparent ? kChromaKey : (dark ? RGB(0x00, 0x00, 0x00) : RGB(0xF3, 0xF3, 0xF3));
    im.SetBackground(background);

    RECT full{0, 0, width, height};
    ::FillRect(back, &full, im.bgBrush.get());

    if (!im.error.empty()) {
        SelectGuard font(back, im.normalFont.get());
        ::SetBkMode(back, TRANSPARENT);
        ::SetTextColor(back, kSystemRed);
        ::DrawTextW(back, im.error.c_str(), static_cast<int>(im.error.size()), &full,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        ::BitBlt(dc, bounds.left, bounds.top, width, height, back, 0, 0, SRCCOPY);
        return;
    }

    im.EnsureLabels(events_, now);

    const LONG gap = Scale(InnerGap(), dpi_);
    const LONG leftWidth = im.frame.left.width;
    const LONG rightWidth = im.frame.right.width;

    RECT strip = full;
    if (leftWidth > 0) strip.left = leftWidth + gap;
    if (rightWidth > 0) strip.right = std::max(strip.left + 1, width - rightWidth - gap);

    // The capsule band is sized to sit alongside the taskbar's icons rather
    // than to fill the bar. A block as tall as the taskbar is legible but looks
    // like a banner pasted over the shell; the same block at icon height reads
    // as another thing living in the tray, which is what it is.
    //
    // Icon height is not a value Windows will tell you -- SM_CYSMICON is the
    // 16 px notification-area icon, not the larger pinned-app icon -- so it is
    // taken as a logical constant scaled to the DPI, clamped to whatever the
    // bar can actually give, and left tunable for a taskbar that has been
    // resized.
    RECT track = strip;
    const LONG stripHeight = RectHeight(strip);
    const LONG wanted = std::max<LONG>(
        1, Scale(Cfg().blockHeight > 0 ? Cfg().blockHeight : kBlockHeight, dpi_));
    const LONG minInset = Scale(kTrackInset, dpi_);
    const LONG bandHeight = std::min(wanted, std::max<LONG>(1, stripHeight - 2 * minInset));
    const LONG inset = std::max(minInset, (stripHeight - bandHeight) / 2);
    track.top += inset;
    track.bottom -= inset;
    if (track.bottom <= track.top) track = strip;

    im.DrawBlocks(back, events_, strip, track, now, dark, background);
    im.DrawNowLine(back, strip, track, dark, background);

    const COLORREF textColour = TaskbarTextColor();
    if (leftWidth > 0) {
        RECT area{0, 0, leftWidth, height};
        im.DrawLabel(back, im.frame.left, area, true,
                     im.frame.left.urgent ? kSystemRed : textColour);
    }
    if (rightWidth > 0) {
        RECT area{width - rightWidth, 0, width, height};
        im.DrawLabel(back, im.frame.right, area, false, textColour);
    }

    ::BitBlt(dc, bounds.left, bounds.top, width, height, back, 0, 0, SRCCOPY);
}

std::wstring Timeline::TooltipText(Seconds now) const {
    Impl& im = *impl_;

    im.pickBuffer.clear();
    for (const auto& e : events_) {
        if (!e.isAllDay && e.end > e.start && e.runningAt(now)) im.pickBuffer.push_back(e);
    }
    const CalEvent* current = timeline::PickChained(im.pickBuffer, true, events_);

    std::wstring text;
    if (current) {
        text = current->title + L" (" + FormatDuration(static_cast<double>(current->end - now)) +
               L")";
    }

    im.pickBuffer.clear();
    for (const auto& e : events_) {
        if (!e.isAllDay && e.end > e.start && e.start >= now) im.pickBuffer.push_back(e);
    }
    const CalEvent* next = timeline::PickChained(im.pickBuffer, false, events_);
    if (next) {
        if (!text.empty()) text += L" | ";
        text += next->title + L" (" + FormatDuration(next->duration()) + L")";
    }

    if (text.empty()) text = kDisplayName;
    return text;
}

// ------------------------------------------------------------- free functions

namespace timeline {

COLORREF ChromaKey() { return kChromaKey; }

const CalEvent* PickChained(const std::vector<CalEvent>& candidates,
                            bool byEndSoonest,
                            const std::vector<CalEvent>& all) {
    // How well an event fits the back-to-back backbone of a time-blocked day:
    // +2 for something else ending where this one starts, +2 for something
    // starting where it ends. It favours the day's own structure over a meeting
    // dropped on top of it, which is nearly always what the user means when
    // they ask what they are doing.
    const auto chainScore = [&all](const CalEvent& e) {
        int score = 0;
        for (const auto& o : all) {
            if (SameEvent(o, e)) continue;
            if (AbsDiff(o.end, e.start) <= kChainTolerance) {
                score += 2;
                break;
            }
        }
        for (const auto& o : all) {
            if (SameEvent(o, e)) continue;
            if (AbsDiff(o.start, e.end) <= kChainTolerance) {
                score += 2;
                break;
            }
        }
        return score;
    };

    const CalEvent* best = nullptr;
    int bestScore = 0;

    for (const auto& e : candidates) {
        if (e.end <= e.start) continue;  // a "(0s)" label helps nobody

        if (!best) {
            best = &e;
            bestScore = chainScore(e);
            continue;
        }

        const Seconds mine = byEndSoonest ? e.end : e.start;
        const Seconds theirs = byEndSoonest ? best->end : best->start;

        // A minute of tolerance, because two blocks meant to be simultaneous
        // are rarely written to the same second. Compared pairwise rather than
        // sorted: a tolerant relation is not transitive, and a sort given one
        // is free to produce any order at all.
        if (AbsDiff(mine, theirs) > kChainTolerance) {
            if (mine < theirs) {
                best = &e;
                bestScore = chainScore(e);
            }
            continue;
        }

        const int score = chainScore(e);
        bool wins;
        if (score != bestScore) {
            wins = score > bestScore;
        } else if (e.duration() != best->duration()) {
            wins = e.duration() < best->duration();
        } else {
            // Alphabetical purely so the choice cannot flicker second to second.
            wins = e.title < best->title;
        }

        if (wins) {
            best = &e;
            bestScore = score;
        }
    }

    return best;
}

int RunningCount(const std::vector<CalEvent>& events, Seconds t) {
    int count = 0;
    for (const auto& e : events) {
        if (e.isAllDay) continue;
        if (e.start <= t && t < NominalEnd(e)) ++count;
    }
    return count;
}

int ClashCount(const std::vector<CalEvent>& events, const CalEvent& next, Seconds now) {
    const Seconds nextEnd = NominalEnd(next);

    int count = 0;
    for (const auto& e : events) {
        if (e.isAllDay) continue;
        const Seconds end = NominalEnd(e);

        // Anything already finished has stopped being a clash. Anything still
        // running is counted, so a meeting dropped inside the block you are in
        // is flagged before it arrives; once it crosses the now line the left
        // gutter picks it up instead, which is what makes the badge's side mean
        // "when".
        if (end <= now) continue;
        if (end > next.start && e.start < nextEnd) ++count;
    }
    return count;
}

}  // namespace timeline
}  // namespace rc
