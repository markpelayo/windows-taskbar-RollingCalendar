// menu.cpp — the menu is the whole user interface.
//
// See menu.h for why it is rebuilt on every open. The consequence worth stating
// here is that nothing in this file may hold state between openings except the
// owner-draw payloads and the cached GDI objects, both of which are managed at
// the bottom of the file.
//
// Three kinds of row are owner-drawn -- the day's blocks, the caption lines and
// the keyword-category rows -- because HMENU text has neither tab stops nor
// inline colour swatches, and those three need both. Everything else is
// ordinary menu text, which costs nothing and behaves correctly under every
// theme, high-contrast setting and screen reader without any help from us.

#include "menu.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "alerts.h"
#include "app.h"
#include "autostart.h"
#include "calsource.h"
#include "common.h"
#include "dialogs.h"
#include "fetch.h"
#include "keywords.h"
#include "resource.h"
#include "settings.h"
#include "soundhours.h"
#include "taskbar.h"
#include "timeline.h"
#include "westminster.h"

namespace rc {
namespace menu {
namespace {

// ---------------------------------------------------------------- punctuation
//
// Built from code points rather than typed literally, so the file stays plain
// ASCII on disk and does not depend on the compiler guessing its encoding.
const wchar_t kSep[] = {L' ', L' ', 0x00B7, L' ', L' ', 0};   // "  ·  "
const wchar_t kBullet[] = {0x2022, 0};                        // "•"
const wchar_t kEnDash[] = {L' ', 0x2013, L' ', 0};            // " – "
const wchar_t kEmDash[] = {L' ', 0x2014, L' ', 0};            // " — "
const wchar_t kPlusMinus[] = {0x00B1, L' ', 0};               // "± "

const COLORREF kBadgeRed = RGB(0xFF, 0x3B, 0x30);

// Every dynamic list is capped at the width of its command-id range in
// resource.h, which is a hundred slots. Nothing on a stock machine comes close
// -- Windows 11 ships somewhere between forty and sixty .wav files -- but a
// user with a folder of imported sounds, or a speech pack with a hundred
// voices, would otherwise run one list's ids into the next one's range. The
// symptom of that is a menu item quietly doing something else entirely, which
// is a great deal worse than a list that stops at a hundred.
const size_t kMaxDynamicItems = 100;

size_t Capped(size_t count) { return count < kMaxDynamicItems ? count : kMaxDynamicItems; }

// The Google embed link people are most likely to be holding when they open
// Add Calendar. It is a placeholder, not a default: nothing is fetched from it.
const wchar_t kLinkExample[] = L"https://calendar.google.com/calendar/embed?src=you@gmail.com";

// ------------------------------------------------------------------- payloads

enum class RowKind { Caption, Day, Category };

// One heap-allocated payload per owner-drawn item, handed to Windows as
// dwItemData. The menu is rebuilt on every open, so each payload has exactly
// one menu's lifetime: Build allocates, ReleaseItemData frees the lot once the
// menu has closed and the last WM_DRAWITEM has been served.
struct ItemData {
    RowKind kind = RowKind::Caption;
    std::wstring text;              // caption text, or a category name
    std::wstring detail;            // the dim tail of a category row
    COLORREF swatch = 0;
    bool hasSwatch = false;
    DayRow row;                     // meaningful only when kind == Day
};

std::vector<ItemData*> g_items;

// ------------------------------------------------------------- cached GDI
//
// Created on first use and kept, keyed on DPI. Creating a font per WM_DRAWITEM
// would mean two GDI objects per row per repaint, and a process is capped at
// ten thousand handles.
struct FontCache {
    int dpi = 0;
    HFONT normal = nullptr;
    HFONT bold = nullptr;
    int lineHeight = 0;
    int capHeight = 0;

    ~FontCache() {
        // Freed at process exit rather than in ReleaseItemData: the fonts are
        // valid for every menu at this DPI, and rebuilding them on each open
        // would defeat the point of caching them at all.
        if (normal) ::DeleteObject(normal);
        if (bold) ::DeleteObject(bold);
    }
};
FontCache g_fonts;

// Column geometry for the day rows, measured once per Build from the widest
// time and duration strings in the whole list. See spec 8.3: padding with
// spaces cannot align proportional text, so the columns are absolute offsets.
struct RowMetrics {
    bool valid = false;
    int pad = 0;
    int gap = 0;
    int marker = 0;
    int swatch = 0;
    int t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0;
    int nameCap = 0;
    int height = 0;
};
RowMetrics g_metrics;

int Scale(int logical) { return ::MulDiv(logical, g_fonts.dpi > 0 ? g_fonts.dpi : 96, 96); }

void EnsureFonts(HWND hwnd) {
    const int dpi = DpiForWindow(hwnd);
    if (g_fonts.normal && g_fonts.dpi == dpi) return;

    if (g_fonts.normal) ::DeleteObject(g_fonts.normal);
    if (g_fonts.bold) ::DeleteObject(g_fonts.bold);
    g_fonts.normal = nullptr;
    g_fonts.bold = nullptr;
    g_fonts.dpi = dpi;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) return;

    // SPI_GETNONCLIENTMETRICS reports the metrics for the system DPI, which is
    // not necessarily this monitor's. Scaling the height by the ratio is what
    // keeps the menu font the same physical size on a second display.
    const HDC screen = ::GetDC(nullptr);
    const int systemDpi = screen ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ::ReleaseDC(nullptr, screen);
    if (systemDpi > 0 && systemDpi != dpi) {
        ncm.lfMenuFont.lfHeight = ::MulDiv(ncm.lfMenuFont.lfHeight, dpi, systemDpi);
    }

    g_fonts.normal = ::CreateFontIndirectW(&ncm.lfMenuFont);

    LOGFONTW boldFont = ncm.lfMenuFont;
    boldFont.lfWeight = FW_BOLD;
    g_fonts.bold = ::CreateFontIndirectW(&boldFont);

    if (!g_fonts.normal) return;
    const HDC dc = ::GetDC(hwnd);
    if (!dc) return;
    const HGDIOBJ old = ::SelectObject(dc, g_fonts.normal);
    TEXTMETRICW tm{};
    ::GetTextMetricsW(dc, &tm);
    g_fonts.lineHeight = tm.tmHeight;
    g_fonts.capHeight = tm.tmAscent - tm.tmInternalLeading;
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd, dc);
}

int TextWidth(HDC dc, const std::wstring& s) {
    if (s.empty()) return 0;
    SIZE size{};
    ::GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &size);
    return size.cx;
}

// ------------------------------------------------------------------ menu glue

// A literal ampersand in a menu string becomes an accelerator underline, and
// calendar profiles, categories and sound files are all user-supplied text.
std::wstring Amp(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 4);
    for (wchar_t c : s) {
        out += c;
        if (c == L'&') out += c;
    }
    return out;
}

void AddSeparator(HMENU m) { ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

void AddText(HMENU m, UINT id, const std::wstring& text, bool checked = false,
             bool enabled = true, bool radio = false) {
    UINT flags = MF_STRING;
    if (checked) flags |= MF_CHECKED;
    if (!enabled) flags |= MF_GRAYED;
    ::AppendMenuW(m, flags, id, Amp(text).c_str());

    if (!radio) return;
    // A radio check has to be asked for on the item itself; MF_CHECKED alone
    // draws a tick, and a tick against six mutually exclusive time ranges reads
    // as "these are options you may combine".
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE;
    mii.fType = MFT_RADIOCHECK;
    ::SetMenuItemInfoW(m, id, FALSE, &mii);
}

// A dim, unclickable line of guidance. Disabled rather than owner-drawn,
// because the shell already knows how to grey a menu item under every theme.
void AddNote(HMENU m, const std::wstring& text) { AddText(m, 0, text, false, false); }

void AddSubmenu(HMENU parent, UniqueMenu child, const std::wstring& text, bool checked = false) {
    UINT flags = MF_POPUP | MF_STRING;
    if (checked) flags |= MF_CHECKED;
    // The parent takes ownership of the submenu handle; DestroyMenu on the root
    // walks the whole tree.
    ::AppendMenuW(parent, flags, reinterpret_cast<UINT_PTR>(child.release()), Amp(text).c_str());
}

void AddOwnerDrawn(HMENU m, UINT id, ItemData* data, bool enabled) {
    g_items.push_back(data);

    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE;
    mii.fType = MFT_OWNERDRAW;
    mii.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
    mii.wID = id;
    mii.dwItemData = reinterpret_cast<ULONG_PTR>(data);
    ::InsertMenuItemW(m, ::GetMenuItemCount(m), TRUE, &mii);
}

void AddCaptionRow(HMENU m, const std::wstring& text) {
    ItemData* data = new ItemData();
    data->kind = RowKind::Caption;
    data->text = text;
    AddOwnerDrawn(m, 0, data, false);
}

// --------------------------------------------------------------- shared lists
//
// Build and Invoke must agree on what index N means. Each dynamic list is
// produced by one function called from both, so an index can only ever mean one
// thing -- the alternative is two copies of the ordering rules drifting apart,
// which shows up as clicking one calendar and getting another.

const double kTimeRanges[] = {10, 20, 30, 60, 120, 240};
const wchar_t* const kTimeRangeNames[] = {L"5 minutes", L"10 minutes", L"15 minutes",
                                          L"30 minutes", L"1 hour", L"2 hours"};
const double kLabelWidths[] = {100, 140, 180, 240, 300, 360, 480};
const int kStartupDelays[] = {5, 10, 15, 20, 30, 60};
const int kVolumePresets[] = {25, 50, 75, 100};
const int kLeadPresets[] = {0, 60, 300, 600};
const wchar_t* const kLeadPresetNames[] = {L"When it starts", L"1 minute before",
                                           L"5 minutes before", L"10 minutes before"};

constexpr int kWidthCount = 8;   // 100..450 step 50
int WidthOption(int index) { return 100 + 50 * index; }

// The two presets first, then whatever the user has added, so the preset rows
// never move under the cursor as customs come and go.
std::vector<SoundWindow> SoundWindowList() {
    std::vector<SoundWindow> list;
    list.push_back(soundhours::kEveningPreset);
    list.push_back(soundhours::kDaytimePreset);
    for (const SoundWindow& w : Cfg().soundHours) {
        if (w.isAllDay()) continue;   // "All day" has its own row
        if (w == soundhours::kEveningPreset || w == soundhours::kDaytimePreset) continue;
        list.push_back(w);
    }
    return list;
}

std::vector<int> LeadList() {
    std::vector<int> list(std::begin(kLeadPresets), std::end(kLeadPresets));
    for (int s : Cfg().alertLeads) {
        if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
    }
    return list;
}

std::vector<int> VolumeList() {
    std::vector<int> list(std::begin(kVolumePresets), std::end(kVolumePresets));
    for (int v : Cfg().chimeCustomVolumes) {
        if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
    }
    return list;
}

std::vector<std::wstring> CategoryList() {
    std::vector<std::wstring> list;
    for (const keywords::CategorySummary& c : keywords::Categories()) list.push_back(c.name);
    list.push_back(keywords::kUncategorized);
    return list;
}

bool IsPresetWindow(const SoundWindow& w) {
    return w == soundhours::kEveningPreset || w == soundhours::kDaytimePreset;
}

// ------------------------------------------------------------------ titles

std::wstring ShortMonth(int month) {
    static const wchar_t* const kNames[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                            L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (month < 1 || month > 12) return L"Jan";
    return kNames[month - 1];
}

void To12Hour(int hour24, int* hour12, const wchar_t** meridiem) {
    *meridiem = hour24 < 12 ? L"AM" : L"PM";
    const int h = hour24 % 12;
    *hour12 = (h == 0) ? 12 : h;
}

std::wstring DebugTimeTitle(const TimeZone& zone) {
    if (!Clock::IsSimulating()) return L"Debug Time...";

    const TimeZone::Parts p = zone.Break(Clock::Now());
    int hour12 = 12;
    const wchar_t* meridiem = L"AM";
    To12Hour(p.hour, &hour12, &meridiem);
    return Format(L"Debug Time: %s %d, %d:%02d:%02d %s...", ShortMonth(p.month).c_str(), p.day,
                  hour12, p.minute, p.second, meridiem);
}

std::wstring VoiceLabel() {
    const std::vector<alerts::Voice> voices = alerts::AvailableVoices();
    for (const alerts::Voice& v : voices) {
        if (v.id == Cfg().alertVoice) return v.label;
    }
    // An empty id means "whatever SAPI defaults to", which is the first voice.
    if (Cfg().alertVoice.empty() && !voices.empty()) return voices.front().label;
    return L"Voice";
}

std::wstring AlertOutputLabel() {
    const Settings& cfg = Cfg();
    if (cfg.alertSpeech) return std::wstring(L"Voice") + kEmDash + VoiceLabel();
    if (cfg.alertSound) return cfg.alertSoundName;
    return L"Off";
}

// Sound Hours is a shared gate, so the two features it silences say so in their
// own titles rather than leaving the user to work out why nothing happened.
std::wstring QuietSuffix(const App& app) {
    if (soundhours::Allows(Clock::Now(), app.Zone())) return std::wstring();
    return std::wstring(kSep) + L"quiet now";
}

// Spec 10: the character count is measured, never hard-coded. A count baked in
// at one DPI and font size is simply wrong at another, and Label Length is the
// one setting whose whole purpose is to say how much text fits.
int CharactersFor(HDC dc, double points) {
    static const wchar_t kSample[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz ";
    if (!dc) return 0;
    const int sampleWidth = TextWidth(dc, kSample);
    const size_t sampleCount = sizeof(kSample) / sizeof(kSample[0]) - 1;   // 53
    if (sampleWidth <= 0 || sampleCount == 0) return 0;

    const double perChar = static_cast<double>(sampleWidth) / static_cast<double>(sampleCount);
    if (perChar <= 0) return 0;
    return static_cast<int>(std::floor(points / perChar + 0.5));
}

// --------------------------------------------------------------- row metrics

void MeasureRows(HWND hwnd, const std::vector<DayRow>& rows) {
    g_metrics = RowMetrics();
    if (!g_fonts.normal) return;

    const HDC dc = ::GetDC(hwnd);
    if (!dc) return;
    const HGDIOBJ old = ::SelectObject(dc, g_fonts.normal);

    g_metrics.pad = Scale(4);
    g_metrics.gap = Scale(7);
    g_metrics.swatch = Scale(10);
    g_metrics.marker = std::max(Scale(9), g_fonts.capHeight * 2 / 3);
    g_metrics.nameCap = Scale(320);
    g_metrics.height = std::max(g_fonts.lineHeight + Scale(4), g_metrics.swatch + Scale(4));

    // The widest string in each column, not an assumed one. Segoe UI's default
    // figures are already tabular at most sizes, so 04:30 does line up under
    // 11:30 -- but where a font's digits differ in width the stops absorb it,
    // because they are derived from the measured maximum rather than assumed.
    int maxTime = 0;
    int maxDuration = 0;
    int maxName = 0;
    for (const DayRow& r : rows) {
        if (r.isSeparator || r.isMore) continue;
        maxTime = std::max(maxTime, TextWidth(dc, r.time));
        maxDuration = std::max(maxDuration, TextWidth(dc, r.duration));
        maxName = std::max(maxName, TextWidth(dc, r.title));
    }
    const int bullet = TextWidth(dc, kBullet);

    g_metrics.t1 = g_metrics.marker + g_metrics.gap;
    g_metrics.t2 = g_metrics.t1 + maxTime + g_metrics.gap;
    g_metrics.t3 = g_metrics.t2 + bullet + g_metrics.gap;
    g_metrics.t4 = g_metrics.t3 + maxDuration + g_metrics.gap;
    g_metrics.t5 = g_metrics.t4 + bullet + g_metrics.gap;

    // A sixth stop, beyond the five the spec names, so the category chips line
    // up as they do in the macOS screenshot. Without it every chip starts
    // wherever its title happened to end and the column looks accidental.
    g_metrics.t6 = g_metrics.t5 + std::min(maxName, g_metrics.nameCap) + g_metrics.gap * 2;
    g_metrics.valid = true;

    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd, dc);
}

// ------------------------------------------------------------------- drawing

COLORREF DimText(bool selected) {
    if (selected) return ::GetSysColor(COLOR_HIGHLIGHTTEXT);
    return Blend(::GetSysColor(COLOR_MENUTEXT), ::GetSysColor(COLOR_GRAYTEXT), 0.65);
}

void DrawSwatch(HDC dc, int x, int centreY, int size, COLORREF colour) {
    const RECT box{x, centreY - size / 2, x + size, centreY - size / 2 + size};
    UniqueBrush fill(::CreateSolidBrush(colour));
    const HGDIOBJ oldBrush = ::SelectObject(dc, fill.get());
    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    // Radius 2.5 at 10 pt, per spec 8.2; RoundRect takes a diameter, and the
    // +1 covers RoundRect excluding its right and bottom edges.
    const int r = std::max(2, size / 4);
    ::RoundRect(dc, box.left, box.top, box.right + 1, box.bottom + 1, r * 2, r * 2);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
}

void DrawTextAt(HDC dc, int x, const RECT& row, const std::wstring& text, COLORREF colour) {
    if (text.empty()) return;
    ::SetTextColor(dc, colour);
    RECT box{x, row.top, row.right, row.bottom};
    // DT_NOPREFIX because an event called "R&D" must not sprout an underline.
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &box,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void DrawDayRow(HDC dc, const DRAWITEMSTRUCT* dis, const ItemData& item) {
    const DayRow& row = item.row;
    const RECT& rc = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const int centreY = (rc.top + rc.bottom) / 2;
    const int x0 = rc.left + g_metrics.pad;

    // Past rows go flat grey; current and future keep the menu's text colour for
    // the title and a blend toward grey for the columns that are context rather
    // than content.
    const COLORREF strong = selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                            : row.isPast ? ::GetSysColor(COLOR_GRAYTEXT)
                                         : ::GetSysColor(COLOR_MENUTEXT);
    const COLORREF dim = selected      ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                         : row.isPast  ? ::GetSysColor(COLOR_GRAYTEXT)
                                       : DimText(false);

    if (row.isSeparator || row.isMore || (row.time.empty() && row.duration.empty())) {
        // Date separators and the "... and N more" tail are headings, not rows:
        // one dim string, no columns to align to.
        const std::wstring text = row.isSeparator ? row.separatorText : row.title;
        DrawTextAt(dc, x0, rc, text, DimText(selected));
        return;
    }

    if (row.isCurrent) {
        // A drawn triangle rather than a glyph: the marker is a shape, and
        // asking a font for it means it disappears on a machine that does not
        // have that font.
        const int size = g_metrics.marker;
        const int top = centreY - size / 2;
        const POINT tri[3] = {{x0, top}, {x0, top + size}, {x0 + size * 3 / 4, centreY}};
        UniqueBrush brush(::CreateSolidBrush(strong));
        const HGDIOBJ oldBrush = ::SelectObject(dc, brush.get());
        const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
        ::Polygon(dc, tri, 3);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
    }

    DrawTextAt(dc, x0 + g_metrics.t1, rc, row.time, dim);
    DrawTextAt(dc, x0 + g_metrics.t2, rc, kBullet, dim);
    DrawTextAt(dc, x0 + g_metrics.t3, rc, row.duration, dim);
    if (!row.duration.empty()) DrawTextAt(dc, x0 + g_metrics.t4, rc, kBullet, dim);

    {
        // The running block's title is the one thing on the row that answers
        // "where am I", so it is the one thing set in bold.
        const HGDIOBJ oldFont =
            ::SelectObject(dc, row.isCurrent && g_fonts.bold ? g_fonts.bold : g_fonts.normal);
        RECT nameBox{x0 + g_metrics.t5, rc.top,
                     std::min<LONG>(rc.right, x0 + g_metrics.t5 + g_metrics.nameCap), rc.bottom};
        ::SetTextColor(dc, strong);
        ::DrawTextW(dc, row.title.c_str(), static_cast<int>(row.title.size()), &nameBox,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(dc, oldFont);
    }

    int x = x0 + g_metrics.t6;
    DrawSwatch(dc, x, centreY, g_metrics.swatch,
               selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : row.categoryColor);
    x += g_metrics.swatch + g_metrics.gap / 2;

    const std::wstring category =
        row.category.empty() ? std::wstring(keywords::kUncategorized) : row.category;
    DrawTextAt(dc, x, rc, category, dim);
    x += TextWidth(dc, category) + g_metrics.gap;

    if (row.overlapCount <= 1) return;

    // The macOS original badges this with a red circle emoji. A drawn dot is
    // used instead because it renders identically at every DPI without
    // depending on an emoji font being installed, and it takes the highlight
    // colour when the row is selected, which an emoji cannot.
    DrawTextAt(dc, x, rc, kBullet, dim);
    x += TextWidth(dc, kBullet) + g_metrics.gap;

    const int dot = std::max(Scale(8), g_fonts.capHeight / 2);
    {
        UniqueBrush brush(
            ::CreateSolidBrush(selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : kBadgeRed));
        const HGDIOBJ oldBrush = ::SelectObject(dc, brush.get());
        const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
        ::Ellipse(dc, x, centreY - dot / 2, x + dot, centreY - dot / 2 + dot);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
    }
    x += dot + g_metrics.gap / 2;

    DrawTextAt(dc, x, rc, Format(L"(%d) Overlapped", row.overlapCount), dim);
}

// ------------------------------------------------------------------ commands

void AfterAppearanceChange(App& app) {
    Cfg().Save();
    app.GetTimeline().InvalidateLabelCache();
    app.RelayoutNow();
    app.InvalidateStrip();
}

// A text-size change needs the fonts rebuilt before anything is re-measured,
// which an ordinary appearance change does not. Measuring with the old font and
// drawing with the new one gives a strip that is the wrong width for one tick,
// and at the sizes involved that is visible as a jump.
void AfterFontChange(App& app) {
    Cfg().Save();
    app.GetTimeline().UpdateFonts(DpiForWindow(app.Window()));
    app.GetTimeline().InvalidateLabelCache();
    app.RelayoutNow();
    app.InvalidateStrip();
}

void AddCalendarFlow(App& app, HWND owner) {
    std::wstring name;
    std::wstring link;

    // Loops rather than returns on a bad link, because the user has just typed
    // or pasted something and losing it to a validation message is the rudest
    // thing a dialog can do.
    for (;;) {
        if (!dialogs::TwoFieldInput(owner, L"Add Calendar", L"Name", L"Work", L"Link",
                                    kLinkExample, &name, &link)) {
            return;
        }
        name = Trim(name);
        link = Trim(link);
        if (link.empty()) return;
        if (name.empty()) name = calsource::Label(link);

        const std::wstring problem = calsource::Problem(link);
        if (problem.empty()) break;
        if (!dialogs::Confirm(owner, L"That link cannot be read", problem, L"Try Again", false)) {
            return;
        }
    }

    Settings& cfg = Cfg();
    cfg.AddProfile(name, link);
    cfg.demoMode = false;
    cfg.Save();
    app.ReloadAfterSourceChange();
}

void RemoveProfileFlow(App& app, HWND owner, const CalendarProfile& profile) {
    Settings& cfg = Cfg();
    const bool onlyOne = cfg.profiles.size() == 1;
    const bool inUse = _wcsicmp(cfg.activeProfile.c_str(), profile.name.c_str()) == 0;

    // Three different things are true about a removal, and which one it is
    // decides whether the user is about to lose their only calendar, swap to
    // another, or tidy up an unused entry.
    std::wstring body;
    if (onlyOne) {
        body = L"This is your only saved calendar. Removing it leaves the strip with nothing "
               L"to show until you add another.";
    } else if (inUse) {
        body = L"This is the calendar currently being read. Removing it switches the strip to "
               L"the first of the ones that remain.";
    } else {
        body = L"This calendar is not the one currently being read, so the strip will not "
               L"change.";
    }
    body += L"\n\nOnly the saved link is forgotten - your calendar itself is untouched.";

    if (!dialogs::Confirm(owner, Format(L"Remove \"%s\"?", profile.name.c_str()), body, L"Remove",
                          true)) {
        return;
    }

    cfg.RemoveProfile(profile.name);
    app.ReloadAfterSourceChange();
}

void ImportKeywordsFlow(App& app, HWND owner) {
    std::wstring path;
    if (!dialogs::OpenFile(owner, L"Import Keyword Colors", L"Comma-separated values",
                           L"*.csv;*.txt", &path)) {
        return;
    }

    std::wstring text;
    if (!ReadFileText(path, &text)) {
        dialogs::Error(owner, L"Import failed", Format(L"Can't read %s", path.c_str()));
        return;
    }

    std::vector<KeywordRule> rules;
    const CsvImportReport report = keywords::ParseCsv(text, &rules);
    if (!report.ok) {
        std::wstring body = report.error;
        if (!report.diagnostics.empty()) body += L"\n\n" + report.diagnostics;
        dialogs::Error(owner, L"Import failed", body);
        return;
    }

    std::wstring source = path;
    const size_t slash = source.find_last_of(L"\\/");
    if (slash != std::wstring::npos) source.erase(0, slash + 1);

    keywords::SetRules(rules, source);
    Settings& cfg = Cfg();
    cfg.keywordRulesSource = source;
    cfg.keywordRulesSeeded = true;
    cfg.SaveKeywordRules(keywords::Rules());
    app.ApplyKeywordRules();

    std::wstring summary = Format(L"%d keywords across %d categories.", report.rulesImported,
                                  report.categories);
    if (report.skippedRows > 0) {
        summary += Format(L"\n%d rows skipped.", report.skippedRows);
    }
    if (!report.duplicates.empty()) {
        summary += Format(L"\n%zu repeated keywords were kept once.", report.duplicates.size());
    }
    if (!report.badColors.empty()) {
        summary += L"\nColours not recognised: ";
        for (size_t i = 0; i < report.badColors.size() && i < 5; ++i) {
            if (i > 0) summary += L", ";
            summary += report.badColors[i];
        }
    }
    summary += L"\n\nWhere two keywords could both match, the longest phrase wins.";
    dialogs::Info(owner, L"Keyword colors imported", summary);
}

void RestoreEverythingFlow(App& app, HWND owner) {
    const std::wstring body =
        L"This clears:\n"
        L"  - the strip's size, time range and labels\n"
        L"  - every saved calendar link\n"
        L"  - imported keyword colours\n"
        L"  - Ending Soon Flash\n"
        L"  - alerts, the chime and Sound Hours\n"
        L"  - Run at Startup and Debug Time\n"
        L"\nOnly saved links are forgotten - your calendars themselves are untouched.";
    if (!dialogs::Confirm(owner, L"Restore everything to defaults?", body, L"Restore Defaults",
                          true)) {
        return;
    }

    // Before the settings file goes, not after: the scheduled task lives in
    // Task Scheduler, and deleting the file that records it would only lose
    // track of it, leaving the app starting itself with no way to say stop.
    std::wstring error;
    autostart::Disable(&error);

    Settings& cfg = Cfg();
    cfg.RestoreAll();

    keywords::SetRules(keywords::SampleRules(), L"the built-in sample");
    cfg.keywordRulesSource = L"the built-in sample";
    cfg.keywordRulesSeeded = true;
    cfg.SaveKeywordRules(keywords::Rules());

    // Same trap as Reset Strip Settings: RestoreAll clears titleFontSize but
    // only UpdateFonts rebuilds the font, and ReloadAfterSourceChange is about
    // the calendar rather than the drawing.
    AfterFontChange(app);
    app.ReloadAfterSourceChange();
}

void SetStartup(HWND owner, bool on, int delaySeconds) {
    Settings& cfg = Cfg();
    std::wstring error;
    const bool ok = on ? autostart::Enable(delaySeconds, &error) : autostart::Disable(&error);

    if (!ok) {
        // The stored flag mirrors the task, never leads it. Claiming the app
        // will start with Windows when the registration failed is worse than
        // saying nothing.
        dialogs::Error(owner, L"Run at Startup", error.empty()
                                                     ? L"Windows would not register the task."
                                                     : error);
        return;
    }

    cfg.runAtStartup = on;
    if (on) cfg.startupDelay = delaySeconds;
    cfg.Save();
}

// Choosing a sound turns speech off and vice versa: two things talking over
// each other is not twice as useful.
void ChooseSound(const std::wstring& name) {
    Settings& cfg = Cfg();
    cfg.alertSoundName = name;
    cfg.alertSound = true;
    cfg.alertSpeech = false;
    cfg.Save();
    alerts::PreviewSound(name);
    soundhours::ArmIfUntouched();
}

void ChooseVoice(const std::wstring& id) {
    Settings& cfg = Cfg();
    cfg.alertVoice = id;
    cfg.alertSpeech = true;
    cfg.alertSound = false;
    cfg.Save();
    soundhours::ArmIfUntouched();
}

// -------------------------------------------------------------- submenu build

UniqueMenu BuildTimeRange() {
    UniqueMenu m(::CreatePopupMenu());
    const double current = Cfg().windowMinutes;
    for (int i = 0; i < static_cast<int>(std::size(kTimeRanges)); ++i) {
        // Tolerance rather than equality: the value has been through a text
        // file, and 120 written and read back is not bit-for-bit 120.
        const bool on = std::fabs(current - kTimeRanges[i]) < 0.01;
        AddText(m.get(), IDM_TIMERANGE_BASE + i,
                std::wstring(kPlusMinus) + kTimeRangeNames[i], on, true, true);
    }
    AddSeparator(m.get());
    AddNote(m.get(), L"A wider range shows more, at a smaller size");
    return m;
}

UniqueMenu BuildTimelineWidth() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();
    for (int i = 0; i < kWidthCount; ++i) {
        const int width = WidthOption(i);
        std::wstring label = Format(L"%d pt", width);
        if (i == 0) label += std::wstring(kEmDash) + L"smallest";
        if (i == kWidthCount - 1) label += std::wstring(kEmDash) + L"largest";
        const bool on = std::fabs(cfg.timelineWidth - width) < 0.01;
        AddText(m.get(), IDM_WIDTH_BASE + i, label, on, true, true);
    }
    AddSeparator(m.get());

    const double minutes = cfg.windowMinutes > 0 ? cfg.windowMinutes : 1;
    const int block = static_cast<int>(std::floor(cfg.timelineWidth / minutes * 15 + 0.5));
    AddNote(m.get(), Format(L"Now: a 15-minute block is %d pt wide", block));
    return m;
}

UniqueMenu BuildLabels() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();
    AddNote(m.get(), L"Left - happening now:");
    AddText(m.get(), IDM_LABEL_NOW_NAME, L"Block name", cfg.showNowName);
    AddText(m.get(), IDM_LABEL_NOW_LEFT, L"Time left", cfg.showNowTimeLeft);
    AddSeparator(m.get());
    AddNote(m.get(), L"Right - up next:");
    AddText(m.get(), IDM_LABEL_NEXT_NAME, L"Block name", cfg.showNextName);
    AddText(m.get(), IDM_LABEL_NEXT_DUR, L"How long it runs", cfg.showNextDuration);
    AddSeparator(m.get());
    AddNote(m.get(), L"Overlap warnings always show");
    return m;
}

UniqueMenu BuildLabelLength(HDC dc) {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();
    for (int i = 0; i < static_cast<int>(std::size(kLabelWidths)); ++i) {
        const double points = kLabelWidths[i];
        std::wstring label = Format(L"%d pt%sabout %d characters", static_cast<int>(points),
                                    kEmDash, CharactersFor(dc, points));
        if (std::fabs(points - 360) < 0.01) label += L" (default)";
        const bool on = std::fabs(cfg.maxLabelWidth - points) < 0.01;
        AddText(m.get(), IDM_LABELWIDTH_BASE + i, label, on, true, true);
    }
    AddSeparator(m.get());
    AddNote(m.get(), L"Longer names are shortened; the time and the overlap badge are never cut");
    return m;
}

UniqueMenu BuildKeywordColors() {
    UniqueMenu m(::CreatePopupMenu());
    const std::vector<keywords::CategorySummary> categories = keywords::Categories();
    const size_t ruleCount = keywords::Rules().size();

    if (ruleCount == 0) {
        AddNote(m.get(), L"No keyword colors");
    } else {
        AddNote(m.get(), Format(L"%zu keywords from %s", ruleCount,
                                keywords::SourceName().c_str()));
        for (const keywords::CategorySummary& c : categories) {
            ItemData* data = new ItemData();
            data->kind = RowKind::Category;
            data->text = c.name;
            data->detail = std::wstring(kSep) + Format(L"%d", c.ruleCount);
            data->swatch = c.color;
            data->hasSwatch = true;
            AddOwnerDrawn(m.get(), 0, data, false);
        }

        ItemData* other = new ItemData();
        other->kind = RowKind::Category;
        other->text = keywords::kUncategorized;
        other->detail = std::wstring(kSep) + L"no keyword match";
        other->swatch = keywords::kUnmatchedColor;
        other->hasSwatch = true;
        AddOwnerDrawn(m.get(), 0, other, false);
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_KEYWORDS_IMPORT,
            ruleCount == 0 ? L"Import CSV..." : L"Import Another CSV...");
    AddText(m.get(), IDM_KEYWORDS_SAMPLE, L"Use Sample Colors");
    AddText(m.get(), IDM_KEYWORDS_SAVE_CSV, L"Save Sample CSV...");
    if (ruleCount > 0) AddText(m.get(), IDM_KEYWORDS_CLEAR, L"Clear Keyword Colors");
    return m;
}

UniqueMenu BuildSavedCalendars() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();

    for (size_t i = 0, n = Capped(cfg.profiles.size()); i < n; ++i) {
        const CalendarProfile& p = cfg.profiles[i];
        const bool active =
            !cfg.demoMode && _wcsicmp(cfg.activeProfile.c_str(), p.name.c_str()) == 0;
        AddText(m.get(), IDM_PROFILE_BASE + static_cast<UINT>(i), p.name, active, true, true);

        // The macOS row carries an inline pencil and cross. An HMENU row cannot
        // carry buttons, so the honest Win32 equivalent is a nested submenu per
        // profile: the same two actions, one level down, and still attached to
        // the row they belong to.
        UniqueMenu edit(::CreatePopupMenu());
        AddText(edit.get(), IDM_PROFILE_RENAME_BASE + static_cast<UINT>(i),
                Format(L"Rename \"%s\"...", p.name.c_str()));
        AddText(edit.get(), IDM_PROFILE_REMOVE_BASE + static_cast<UINT>(i),
                Format(L"Remove \"%s\"...", p.name.c_str()));
        AddSubmenu(m.get(), std::move(edit), Format(L"Edit \"%s\"", p.name.c_str()));
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_ADD_CALENDAR, L"Add Calendar...");
    return m;
}

UniqueMenu BuildSoundHours() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();
    const bool gateOn = cfg.soundHoursOn && !cfg.soundHours.empty();

    AddText(m.get(), IDM_SOUNDHOURS_OFF, L"Off", !gateOn, true, true);
    AddSeparator(m.get());

    const std::vector<SoundWindow> list = SoundWindowList();
    for (size_t i = 0; i < list.size(); ++i) {
        const SoundWindow& w = list[i];
        // While the gate is off every row reads unticked, which is the honest
        // picture: none of them are in force.
        const bool on = gateOn && std::find(cfg.soundHours.begin(), cfg.soundHours.end(), w) !=
                                      cfg.soundHours.end();
        AddText(m.get(), IDM_SOUNDWINDOW_BASE + static_cast<UINT>(i), soundhours::Describe(w), on);

        if (IsPresetWindow(w)) continue;
        // Only a window the user added can be deleted; the two presets are
        // always offered, ticked or not.
        AddText(m.get(), IDM_SOUNDWINDOW_DEL_BASE + static_cast<UINT>(i),
                Format(L"     Remove %s", soundhours::Describe(w).c_str()));
    }

    bool allDay = false;
    for (const SoundWindow& w : cfg.soundHours) {
        if (w.isAllDay()) allDay = true;
    }
    AddText(m.get(), IDM_SOUNDHOURS_ALLDAY, L"All day", gateOn && allDay);
    AddSeparator(m.get());
    AddText(m.get(), IDM_SOUNDHOURS_CUSTOM, L"Add Custom...");
    return m;
}

UniqueMenu BuildAlertMe() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();

    AddText(m.get(), IDM_ALERTS_OFF, L"Off", cfg.alertLeads.empty(), true, true);
    AddSeparator(m.get());

    const std::vector<int> leads = LeadList();
    for (size_t i = 0; i < leads.size(); ++i) {
        const int seconds = leads[i];
        std::wstring label;
        if (i < std::size(kLeadPresetNames) && seconds == kLeadPresets[i]) {
            label = kLeadPresetNames[i];
        } else {
            label = alerts::LeadPhrase(seconds) + L" before";
        }
        const bool on = std::find(cfg.alertLeads.begin(), cfg.alertLeads.end(), seconds) !=
                        cfg.alertLeads.end();
        AddText(m.get(), IDM_LEAD_BASE + static_cast<UINT>(i), label, on);
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_ALERTS_CUSTOM_LEAD, L"Add Custom...");
    return m;
}

UniqueMenu BuildAlertSound() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();

    AddText(m.get(), IDM_ALERTS_SOUND_OFF, L"Off", !cfg.alertSound, true, true);
    AddSeparator(m.get());

    const std::vector<std::wstring> sounds = alerts::AvailableSounds();
    for (size_t i = 0, n = Capped(sounds.size()); i < n; ++i) {
        const bool on = cfg.alertSound && cfg.alertSoundName == sounds[i];
        AddText(m.get(), IDM_SOUNDNAME_BASE + static_cast<UINT>(i), sounds[i], on, true, true);
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_ALERTS_SOUND_IMPORT, L"Custom Sound...");
    return m;
}

UniqueMenu BuildAlertVoice() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();

    AddText(m.get(), IDM_ALERTS_VOICE_OFF, L"Off", !cfg.alertSpeech, true, true);
    AddSeparator(m.get());

    const std::vector<alerts::Voice> voices = alerts::AvailableVoices();
    for (size_t i = 0, n = Capped(voices.size()); i < n; ++i) {
        const bool on = cfg.alertSpeech && (cfg.alertVoice == voices[i].id ||
                                            (cfg.alertVoice.empty() && i == 0));
        AddText(m.get(), IDM_VOICE_BASE + static_cast<UINT>(i), voices[i].label, on, true, true);
    }
    if (voices.empty()) AddNote(m.get(), L"No speech voices installed");

    AddSeparator(m.get());
    AddText(m.get(), IDM_ALERTS_VOICE_MANAGE, L"Manage Voices...");
    return m;
}

// ------------------------------------------------------------------ text size

// Three steps above the shell's size, which is what the default is. Chosen as
// noticeable rather than fine-grained: anyone who wants 11.5 pt can add it, and
// a submenu of every size between 9 and 16 would be a scrolling list nobody
// reads.
const double kFontSizeChoices[] = {11.0, 13.0, 15.0};

// See the comment where this is used: the three text-size id ranges are twenty
// apart, so the user's own list has to stop at twenty.
constexpr size_t kMaxFontSizes = 20;

std::wstring FontSizeLabel(double points) {
    if (points <= 0) return L"Default (matches the taskbar)";
    return Format(L"%g pt", points);
}

std::wstring FontSizeMenuTitle() {
    const double s = Cfg().titleFontSize;
    return L"Text Size: " + (s > 0 ? Format(L"%g pt", s) : std::wstring(L"Default"));
}

UniqueMenu BuildFontSize() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();
    const double current = cfg.titleFontSize;

    AddText(m.get(), IDM_FONTSIZE_DEFAULT, FontSizeLabel(0), current <= 0, true, true);
    AddSeparator(m.get());

    for (size_t i = 0; i < std::size(kFontSizeChoices); ++i) {
        const bool on = std::fabs(current - kFontSizeChoices[i]) < 0.01;
        AddText(m.get(), IDM_FONTSIZE_BASE + static_cast<UINT>(i),
                FontSizeLabel(kFontSizeChoices[i]), on, true, true);
    }

    // The user's own sizes, each with a Remove beside it. A list you can add to
    // but not take from fills up permanently, and the only way out would be to
    // edit the settings file.
    if (!cfg.fontSizeCustoms.empty()) {
        AddSeparator(m.get());
        // Capped at the width of its own id range, which is narrower than the
        // general dynamic-list cap: the three font-size ranges are twenty slots
        // apart, so a twenty-first custom size would collide with the next
        // range and quietly become a different command.
        const size_t n = std::min<size_t>(cfg.fontSizeCustoms.size(), kMaxFontSizes);
        for (size_t i = 0; i < n; ++i) {
            const double size = cfg.fontSizeCustoms[i];
            const bool on = std::fabs(current - size) < 0.01;
            AddText(m.get(), IDM_FONTSIZE_CUST_BASE + static_cast<UINT>(i), FontSizeLabel(size),
                    on, true, true);
            AddText(m.get(), IDM_FONTSIZE_DEL_BASE + static_cast<UINT>(i),
                    L"      Remove " + FontSizeLabel(size));
        }
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_FONTSIZE_CUSTOM, L"Add Custom...");
    AddNote(m.get(), L"Reset Strip Settings puts this back to the default");
    return m;
}

// ---------------------------------------------------------- ending soon flash

// One, two and five minutes. Ten was offered briefly in the original and
// dropped: a name blinking for ten minutes stops being a warning and becomes
// the strip's normal appearance.
const int kFlashChoices[] = {60, 120, 300};

// "90 sec", "5 min", "1.5 min". Whole minutes read as minutes; anything below
// one minute reads in seconds, because a custom 90 seconds rounding to "2 min"
// would be a lie about the setting the user just typed.
std::wstring FlashPhrase(double seconds) {
    if (seconds <= 0) return L"Off";
    if (seconds < 60) return Format(L"%g sec", seconds);
    const double minutes = seconds / 60.0;
    if (std::fabs(minutes - std::floor(minutes)) < 0.001) {
        return Format(L"%d min", static_cast<int>(std::lround(minutes)));
    }
    return Format(L"%g min", minutes);
}

std::wstring FlashMenuTitle() {
    const double s = Cfg().endingFlashSeconds;
    if (s <= 0) return L"Ending Soon Flash: Off";
    return L"Ending Soon Flash: " + FlashPhrase(s) + L" before the end";
}

UniqueMenu BuildEndingSoonFlash() {
    UniqueMenu m(::CreatePopupMenu());
    const double current = Cfg().endingFlashSeconds;

    AddText(m.get(), IDM_FLASH_OFF, L"Off", current <= 0, true, true);
    AddSeparator(m.get());

    bool matched = false;
    for (size_t i = 0; i < std::size(kFlashChoices); ++i) {
        const double preset = static_cast<double>(kFlashChoices[i]);
        const bool on = std::fabs(current - preset) < 0.01;
        if (on) matched = true;
        AddText(m.get(), IDM_FLASH_BASE + static_cast<UINT>(i),
                FlashPhrase(preset) + L" before the end", on, true, true);
    }

    // A custom value that is not one of the presets gets its own ticked row, so
    // the setting in force is never invisible.
    if (current > 0 && !matched) {
        AddSeparator(m.get());
        AddText(m.get(), IDM_FLASH_CUSTOM, FlashPhrase(current) + L" before the end", true, true,
                true);
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_FLASH_CUSTOM, L"Add Custom...");
    AddNote(m.get(), L"The name blinks red; Reset Strip Settings leaves this alone");
    return m;
}

// Which display's taskbar the strip lives in.
//
// Windows only creates a taskbar on a secondary display when "Show my taskbar
// on all displays" is on, so this list is a list of *taskbars*, not of
// monitors. If a display the user expects is missing, the setting to change is
// in Windows, not here, and the note at the bottom says so.
UniqueMenu BuildDisplays(const std::vector<TaskbarInfo>& bars) {
    UniqueMenu m(::CreatePopupMenu());
    const std::wstring& chosen = Cfg().monitorDevice;

    AddText(m.get(), IDM_MONITOR_AUTO, L"Primary display (automatic)", chosen.empty(), true, true);
    AddSeparator(m.get());

    for (size_t i = 0, n = Capped(bars.size()); i < n; ++i) {
        const bool on = !chosen.empty() &&
                        _wcsicmp(chosen.c_str(), bars[i].monitorDevice.c_str()) == 0;
        AddText(m.get(), IDM_MONITOR_BASE + static_cast<UINT>(i), bars[i].MonitorLabel(), on, true,
                true);
    }

    AddSeparator(m.get());
    AddNote(m.get(), L"Only displays showing a taskbar are listed");
    return m;
}

std::wstring CurrentDisplayLabel(const std::vector<TaskbarInfo>& bars) {
    const std::wstring& chosen = Cfg().monitorDevice;
    for (const TaskbarInfo& bar : bars) {
        const bool match = chosen.empty() ? bar.isPrimaryMonitor
                                          : _wcsicmp(chosen.c_str(),
                                                     bar.monitorDevice.c_str()) == 0;
        if (match) return bar.MonitorLabel();
    }
    // The chosen display has no taskbar right now -- a dock was unplugged. Say
    // so rather than showing a label for somewhere the strip is not.
    return L"primary (chosen display absent)";
}

UniqueMenu BuildAlertCategories() {
    UniqueMenu m(::CreatePopupMenu());
    AddText(m.get(), IDM_ALERTS_CATS_ALL, L"All Categories", Cfg().alertCategories.empty(), true,
            true);
    AddSeparator(m.get());

    const std::vector<std::wstring> categories = CategoryList();
    for (size_t i = 0, n = Capped(categories.size()); i < n; ++i) {
        AddText(m.get(), IDM_CATEGORY_BASE + static_cast<UINT>(i), categories[i],
                alerts::CategoryAllowed(categories[i]));
    }
    return m;
}

UniqueMenu BuildAlerts() {
    UniqueMenu m(::CreatePopupMenu());
    AddSubmenu(m.get(), BuildAlertMe(), L"Alert Me: " + alerts::DescribeLeads());
    AddSeparator(m.get());

    const Settings& cfg = Cfg();
    AddSubmenu(m.get(), BuildAlertSound(),
               L"Alert Sound: " + (cfg.alertSound ? cfg.alertSoundName : std::wstring(L"Off")));
    AddSubmenu(m.get(), BuildAlertVoice(),
               L"Voice: " + (cfg.alertSpeech ? VoiceLabel() : std::wstring(L"Off")));
    AddSeparator(m.get());

    AddSubmenu(m.get(), BuildAlertCategories(), L"Categories: " + alerts::DescribeCategories());
    AddSeparator(m.get());
    AddText(m.get(), IDM_ALERTS_TEST, L"Test Alert Now");
    return m;
}

UniqueMenu BuildChimeVolume() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();
    const std::vector<int> volumes = VolumeList();

    for (size_t i = 0; i < volumes.size(); ++i) {
        const int v = volumes[i];
        const bool on = std::fabs(static_cast<double>(cfg.chimeVolume) * 100 - v) < 0.5;
        AddText(m.get(), IDM_CHIMEVOL_BASE + static_cast<UINT>(i), Format(L"%d%%", v), on, true,
                true);
        if (i < std::size(kVolumePresets)) continue;
        AddText(m.get(), IDM_CHIMEVOL_DEL_BASE + static_cast<UINT>(i),
                Format(L"     Remove %d%%", v));
    }

    AddSeparator(m.get());
    AddText(m.get(), IDM_CHIME_VOL_CUSTOM, L"Add Custom...");
    return m;
}

UniqueMenu BuildChimeHearIt() {
    UniqueMenu m(::CreatePopupMenu());
    AddText(m.get(), IDM_CHIME_HEAR_PAST, L"Quarter past");
    AddText(m.get(), IDM_CHIME_HEAR_HALF, L"Half past");
    AddText(m.get(), IDM_CHIME_HEAR_TO, L"Quarter to");
    AddText(m.get(), IDM_CHIME_HEAR_HOUR, L"The hour");
    return m;
}

UniqueMenu BuildChime() {
    UniqueMenu m(::CreatePopupMenu());
    const Settings& cfg = Cfg();

    AddText(m.get(), IDM_CHIME_OFF, L"Off", cfg.chimeMode == ChimeMode::Off, true, true);
    AddText(m.get(), IDM_CHIME_HOURLY, L"On the hour", cfg.chimeMode == ChimeMode::Hourly, true,
            true);
    AddText(m.get(), IDM_CHIME_QUARTERLY, L"Every quarter hour",
            cfg.chimeMode == ChimeMode::Quarterly, true, true);
    AddSeparator(m.get());

    AddText(m.get(), IDM_CHIME_STRIKE_HOUR, L"Strike the Hour Count", cfg.chimeStrikesHour);
    AddSubmenu(m.get(), BuildChimeVolume(),
               Format(L"Volume: %d%%", static_cast<int>(cfg.chimeVolume * 100 + 0.5f)));
    AddSeparator(m.get());

    AddSubmenu(m.get(), BuildChimeHearIt(), L"Hear It");
    AddText(m.get(), IDM_CHIME_STOP, L"Stop Ringing", false, westminster::IsRinging());
    AddSeparator(m.get());
    AddNote(m.get(),
            L"The Westminster Quarters, synthesised - no recordings, nothing to download");
    return m;
}

UniqueMenu BuildStartup() {
    UniqueMenu m(::CreatePopupMenu());
    const bool on = autostart::IsEnabled();
    AddText(m.get(), IDM_STARTUP_OFF, L"Off", !on, true, true);
    AddText(m.get(), IDM_STARTUP_ON, L"On", on, true, true);
    AddSeparator(m.get());

    AddNote(m.get(), L"Delay for:");
    const int delay = Cfg().startupDelay;
    for (int i = 0; i < static_cast<int>(std::size(kStartupDelays)); ++i) {
        AddText(m.get(), IDM_STARTUP_DELAY_BASE + i, Format(L"%d s", kStartupDelays[i]),
                delay == kStartupDelays[i], true, true);
    }
    return m;
}

}  // namespace

// ---------------------------------------------------------------------- Build

UniqueMenu Build(App& app, const std::vector<DayRow>& rows) {
    // Any payload from the previous opening is dead by now; the menu that
    // referred to it has been destroyed.
    ReleaseItemData();

    const HWND owner = app.Window();
    EnsureFonts(owner);
    MeasureRows(owner, rows);

    // One measuring DC for the whole build. The character counts under Label
    // Length are the only thing that needs it, and opening one per row would be
    // eight DCs for one submenu.
    const HDC dc = ::GetDC(owner);
    const HGDIOBJ oldFont = (dc && g_fonts.normal) ? ::SelectObject(dc, g_fonts.normal) : nullptr;

    UniqueMenu m(::CreatePopupMenu());
    Settings& cfg = Cfg();

    // 1-2: who wrote this and where it lives.
    AddText(m.get(), IDM_PROJECT_PAGE,
            std::wstring(kDisplayName) + L" " + kVersion + kSep + L"by markpelayo");
    AddSeparator(m.get());

    // 3-6: what day it is, and the simulated clock.
    AddCaptionRow(m.get(), daylist::Caption(Clock::Now(), app.Zone(), app.SourceName()));
    AddText(m.get(), IDM_DEBUG_TIME, DebugTimeTitle(app.Zone()));
    if (Clock::IsSimulating()) AddText(m.get(), IDM_DEBUG_RESET, L"Reset to Current Time");
    AddSeparator(m.get());

    // 7: the day itself. Disabled but drawn normally -- these are a listing,
    // not commands, and greying them out would wash the colour swatches to
    // nothing, which is the one thing they are there for.
    if (!app.ErrorMessage().empty()) {
        DayRow error;
        error.title = app.ErrorMessage();
        ItemData* data = new ItemData();
        data->kind = RowKind::Day;
        data->row = error;
        AddOwnerDrawn(m.get(), IDM_DAYROW_BASE, data, false);
    } else if (rows.empty()) {
        DayRow empty;
        empty.title = L"Nothing scheduled";
        ItemData* data = new ItemData();
        data->kind = RowKind::Day;
        data->row = empty;
        AddOwnerDrawn(m.get(), IDM_DAYROW_BASE, data, false);
    } else {
        for (size_t i = 0; i < rows.size(); ++i) {
            ItemData* data = new ItemData();
            data->kind = RowKind::Day;
            data->row = rows[i];
            AddOwnerDrawn(m.get(), IDM_DAYROW_BASE + static_cast<UINT>(i), data, false);
        }
    }
    AddSeparator(m.get());

    // 9-13: what the blocks are -- where they come from, what colour they are,
    // and how loudly the one you are in says it is nearly over. Keyword Colors
    // used to sit below with the geometry; it belongs here, because it is about
    // the events rather than the timeline's proportions.
    AddText(m.get(), IDM_DEMO_CALENDAR, L"Demo Calendar", cfg.demoMode);
    if (cfg.profiles.empty()) {
        AddText(m.get(), IDM_ADD_CALENDAR, L"Add Calendar...");
    } else {
        AddSubmenu(m.get(), BuildSavedCalendars(), L"Saved Calendars");
    }
    AddSubmenu(m.get(), BuildKeywordColors(), L"Keyword Colors");
    AddSubmenu(m.get(), BuildEndingSoonFlash(), FlashMenuTitle(), cfg.isFlashing());
    AddSeparator(m.get());

    // 15-19: geometry, and nothing but geometry -- which is what makes Reset
    // Strip Settings a safe click. Renamed from "Restore Strip Settings": two
    // rows in one menu both starting with "Restore" read as the same action
    // twice, and the heavier of the two is the one further down.
    AddSubmenu(m.get(), BuildTimeRange(), L"Time Range");
    AddSubmenu(m.get(), BuildTimelineWidth(), L"Timeline Width");
    AddSubmenu(m.get(), BuildLabels(), L"Labels");
    AddSubmenu(m.get(), BuildLabelLength(dc), L"Label Length");
    AddSubmenu(m.get(), BuildFontSize(), FontSizeMenuTitle());
    AddText(m.get(), IDM_RESTORE_STRIP, L"Reset Strip Settings", false,
            !cfg.isAppearanceDefault());
    AddSeparator(m.get());

    // 19-21: the noises, and the one schedule that gates them both.
    const std::wstring quiet = QuietSuffix(app);
    AddSubmenu(m.get(), BuildSoundHours(), L"Sound Hours: " + soundhours::DescribeCurrent(),
               cfg.soundHoursOn && !cfg.soundHours.empty());
    AddSubmenu(m.get(), BuildAlerts(),
               L"Time Block Alerts: " + alerts::DescribeLeads() + L" | " + AlertOutputLabel() +
                   quiet,
               cfg.alertsEnabled());
    AddSubmenu(m.get(), BuildChime(),
               L"Westminster Chime: " + westminster::DescribeMode(cfg.chimeMode) + quiet,
               cfg.chimeMode != ChimeMode::Off);
    AddSeparator(m.get());

    // 23-24: how old the data is.
    AddCaptionRow(m.get(), daylist::FreshnessCaption(app.LastFetch(), IsFetching()));
    AddText(m.get(), IDM_REFRESH_NOW, IsFetching() ? L"Refreshing..." : L"Refresh Now", false,
            !IsFetching());
    AddSeparator(m.get());

    // 26: where the widget sits.
    //
    // The display submenu only appears when there is more than one taskbar to
    // choose between. On a single-monitor machine, or one where the taskbar is
    // shown on the primary display only, a menu offering a choice of one would
    // be a puzzle rather than a setting.
    {
        const std::vector<TaskbarInfo> bars = EnumerateTaskbars();
        if (bars.size() > 1) {
            AddSubmenu(m.get(), BuildDisplays(bars), L"Show on Display: " + CurrentDisplayLabel(bars));
        }
    }
    AddText(m.get(), IDM_MOVE_WIDGET, L"Move widget...");
    AddText(m.get(), IDM_RESET_POSITION, L"Reset widget position");
    AddSeparator(m.get());

    // 28-32.
    AddSubmenu(m.get(), BuildStartup(), L"Run at Startup: " + autostart::Describe());
    AddSeparator(m.get());
    AddText(m.get(), IDM_RESTORE_ALL, L"Restore Defaults...", false, !cfg.isEverythingDefault());
    AddSeparator(m.get());
    AddText(m.get(), IDM_QUIT, std::wstring(L"Quit ") + kDisplayName);

    if (dc) {
        if (oldFont) ::SelectObject(dc, oldFont);
        ::ReleaseDC(owner, dc);
    }
    return m;
}

// -------------------------------------------------------------- owner drawing

void OnMeasureItem(HWND hwnd, MEASUREITEMSTRUCT* mis) {
    if (!mis || mis->CtlType != ODT_MENU) return;
    const ItemData* item = reinterpret_cast<const ItemData*>(mis->itemData);
    if (!item) return;

    EnsureFonts(hwnd);
    const HDC dc = ::GetDC(hwnd);
    if (!dc || !g_fonts.normal) {
        if (dc) ::ReleaseDC(hwnd, dc);
        return;
    }
    const HGDIOBJ old = ::SelectObject(dc, g_fonts.normal);

    const int pad = std::max(g_metrics.pad, Scale(4));
    int width = pad * 2;
    int height = std::max(g_metrics.height, g_fonts.lineHeight + Scale(4));

    switch (item->kind) {
        case RowKind::Caption:
            width += TextWidth(dc, item->text);
            break;

        case RowKind::Category:
            width += g_metrics.swatch + g_metrics.gap + TextWidth(dc, item->text) +
                     TextWidth(dc, item->detail);
            break;

        case RowKind::Day: {
            const DayRow& row = item->row;
            if (!g_metrics.valid) {
                width += TextWidth(dc, row.title);
                break;
            }
            if (row.isSeparator || row.isMore || (row.time.empty() && row.duration.empty())) {
                width += TextWidth(dc, row.isSeparator ? row.separatorText : row.title);
                break;
            }

            const std::wstring category =
                row.category.empty() ? std::wstring(keywords::kUncategorized) : row.category;
            width += g_metrics.t6 + g_metrics.swatch + g_metrics.gap / 2 +
                     TextWidth(dc, category) + g_metrics.gap;
            if (row.overlapCount > 1) {
                width += TextWidth(dc, kBullet) + g_metrics.gap +
                         std::max(Scale(8), g_fonts.capHeight / 2) + g_metrics.gap / 2 +
                         TextWidth(dc, Format(L"(%d) Overlapped", row.overlapCount));
            }
            break;
        }
    }

    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd, dc);

    mis->itemWidth = static_cast<UINT>(std::max(width, Scale(80)));
    mis->itemHeight = static_cast<UINT>(std::max(height, Scale(16)));
}

void OnDrawItem(HWND hwnd, DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU || !dis->hDC) return;
    const ItemData* item = reinterpret_cast<const ItemData*>(dis->itemData);
    if (!item) return;

    EnsureFonts(hwnd);
    if (!g_fonts.normal) return;

    const HDC dc = dis->hDC;
    DcStateGuard guard(dc);
    ::SetBkMode(dc, TRANSPARENT);
    ::SelectObject(dc, g_fonts.normal);

    // The day rows and the captions are all disabled, so they will never be
    // selected. Handled anyway: an owner-draw handler that assumes a state
    // cannot happen is one theme change away from drawing invisible text.
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    ::FillRect(dc, &dis->rcItem,
               reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(
                   (selected ? COLOR_HIGHLIGHT : COLOR_MENU) + 1)));

    const int pad = std::max(g_metrics.pad, Scale(4));

    switch (item->kind) {
        case RowKind::Caption:
            DrawTextAt(dc, dis->rcItem.left + pad, dis->rcItem, item->text, DimText(selected));
            break;

        case RowKind::Category: {
            const int centreY = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            int x = dis->rcItem.left + pad;
            DrawSwatch(dc, x, centreY, g_metrics.swatch ? g_metrics.swatch : Scale(10),
                       item->swatch);
            x += (g_metrics.swatch ? g_metrics.swatch : Scale(10)) + Scale(7);
            DrawTextAt(dc, x, dis->rcItem, item->text + item->detail, DimText(selected));
            break;
        }

        case RowKind::Day:
            if (!g_metrics.valid) {
                DrawTextAt(dc, dis->rcItem.left + pad, dis->rcItem, item->row.title,
                           DimText(selected));
                break;
            }
            DrawDayRow(dc, dis, *item);
            break;
    }
}

void ReleaseItemData() {
    for (ItemData* item : g_items) delete item;
    g_items.clear();
    g_metrics = RowMetrics();
    // The fonts deliberately survive: they depend only on the DPI and the
    // shell's menu font, neither of which changes between one opening and the
    // next. They are released by FontCache's destructor at process exit.
}

// --------------------------------------------------------------------- Invoke

bool Invoke(App& app, HWND owner, UINT id) {
    Settings& cfg = Cfg();

    switch (id) {
        case IDM_PROJECT_PAGE:
            ::ShellExecuteW(owner, L"open", kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return true;

        case IDM_DEBUG_TIME: {
            Seconds picked = 0;
            const int result = dialogs::DebugTimePicker(owner, app.Zone(), Clock::Now(), &picked);
            if (result == 1) {
                // Stored as an offset rather than an instant, so the simulated
                // clock keeps advancing and countdowns still tick.
                Clock::SetOffset(static_cast<double>(picked - RealNow()));
            } else if (result == 2) {
                Clock::SetOffset(0);
            } else {
                return true;
            }
            cfg.debugOffset = Clock::Offset();
            cfg.Save();
            app.InvalidateStrip();
            return true;
        }

        case IDM_DEBUG_RESET:
            Clock::SetOffset(0);
            cfg.debugOffset = 0;
            cfg.Save();
            app.InvalidateStrip();
            return true;

        case IDM_DEMO_CALENDAR:
            cfg.demoMode = !cfg.demoMode;
            cfg.Save();
            app.ReloadAfterSourceChange();
            return true;

        case IDM_ADD_CALENDAR:
            AddCalendarFlow(app, owner);
            return true;

        case IDM_RESTORE_STRIP:
            cfg.RestoreStrip();
            // AfterFontChange, not AfterAppearanceChange: the reset puts
            // titleFontSize back to zero, and the font object is only rebuilt
            // by UpdateFonts. Without this the setting resets and the strip
            // carries on drawing at the old size, which looks exactly like the
            // reset having ignored the text size.
            AfterFontChange(app);
            return true;

        case IDM_RESTORE_ALL:
            RestoreEverythingFlow(app, owner);
            return true;

        case IDM_REFRESH_NOW:
            app.Refresh();
            return true;

        case IDM_FONTSIZE_DEFAULT:
            cfg.titleFontSize = 0;
            cfg.Save();
            AfterFontChange(app);
            return true;

        case IDM_FONTSIZE_CUSTOM: {
            double points = cfg.titleFontSize > 0 ? cfg.titleFontSize : 12.0;
            if (!dialogs::NumberInput(owner, L"Text Size",
                                      L"Strip text size in points, from 6 to 48.", 6.0, 48.0,
                                      false, &points)) {
                return true;
            }
            cfg.titleFontSize = points;

            // Remembered as well as applied, so it can be chosen again later
            // without retyping. Presets are not duplicated into the list.
            bool known = false;
            for (double preset : kFontSizeChoices) {
                if (std::fabs(preset - points) < 0.01) known = true;
            }
            for (double existing : cfg.fontSizeCustoms) {
                if (std::fabs(existing - points) < 0.01) known = true;
            }
            if (!known && cfg.fontSizeCustoms.size() < kMaxFontSizes) {
                cfg.fontSizeCustoms.push_back(points);
                std::sort(cfg.fontSizeCustoms.begin(), cfg.fontSizeCustoms.end());
            }
            cfg.Save();
            AfterFontChange(app);
            return true;
        }

        case IDM_FLASH_OFF:
            cfg.endingFlashSeconds = 0;
            cfg.Save();
            AfterAppearanceChange(app);
            return true;

        case IDM_FLASH_CUSTOM: {
            // Replaces the value rather than joining a set. Unlike alert lead
            // times, where ten minutes before and one minute before are both
            // useful, there is only one answer to how long a name should blink.
            double minutes = cfg.isFlashing() ? cfg.endingFlashSeconds / 60.0 : 3.0;
            if (!dialogs::NumberInput(owner, L"Ending Soon Flash",
                                      L"How long before a block ends should its name start "
                                      L"flashing? Minutes, from 0.25 to 60.",
                                      0.25, 60.0, false, &minutes)) {
                return true;
            }
            cfg.endingFlashSeconds = minutes * 60.0;
            cfg.Save();
            AfterAppearanceChange(app);
            return true;
        }

        case IDM_MONITOR_AUTO:
            // Back to the primary display, and the dragged position goes with
            // it: an offset measured along one taskbar means nothing on
            // another, and leaving it would park the strip at an arbitrary
            // point on the new one.
            cfg.monitorDevice.clear();
            cfg.widgetOffsetFromRight = -1;
            cfg.Save();
            app.RelocateToTaskbar();
            return true;

        case IDM_MOVE_WIDGET:
            app.BeginMoveWidget();
            return true;

        case IDM_RESET_POSITION:
            app.ResetWidgetPosition();
            return true;

        case IDM_QUIT:
            app.Quit();
            return true;

        // ---- labels ----------------------------------------------------
        case IDM_LABEL_NOW_NAME:
            cfg.showNowName = !cfg.showNowName;
            AfterAppearanceChange(app);
            return true;
        case IDM_LABEL_NOW_LEFT:
            cfg.showNowTimeLeft = !cfg.showNowTimeLeft;
            AfterAppearanceChange(app);
            return true;
        case IDM_LABEL_NEXT_NAME:
            cfg.showNextName = !cfg.showNextName;
            AfterAppearanceChange(app);
            return true;
        case IDM_LABEL_NEXT_DUR:
            cfg.showNextDuration = !cfg.showNextDuration;
            AfterAppearanceChange(app);
            return true;

        // ---- keyword colours -------------------------------------------
        case IDM_KEYWORDS_IMPORT:
            ImportKeywordsFlow(app, owner);
            return true;

        case IDM_KEYWORDS_SAMPLE:
            keywords::SetRules(keywords::SampleRules(), L"the built-in sample");
            cfg.keywordRulesSource = L"the built-in sample";
            cfg.keywordRulesSeeded = true;
            cfg.SaveKeywordRules(keywords::Rules());
            app.ApplyKeywordRules();
            return true;

        case IDM_KEYWORDS_SAVE_CSV: {
            std::wstring path;
            if (!dialogs::SaveFile(owner, L"Save Sample CSV", L"rolling-calendar-colors.csv",
                                   L"Comma-separated values", L"*.csv", &path)) {
                return true;
            }
            if (!WriteFileText(path, keywords::SampleCsv())) {
                dialogs::Error(owner, L"Save failed", Format(L"Can't write %s", path.c_str()));
            }
            return true;
        }

        case IDM_KEYWORDS_CLEAR:
            keywords::Clear();
            // The seeded flag stays set, which is what makes cleared stay
            // cleared across a relaunch rather than reappearing on next start.
            cfg.keywordRulesSource.clear();
            cfg.keywordRulesSeeded = true;
            cfg.SaveKeywordRules(std::vector<KeywordRule>());
            app.ApplyKeywordRules();
            return true;

        // ---- sound hours -------------------------------------------------
        case IDM_SOUNDHOURS_OFF:
            cfg.soundHoursTouched = true;
            soundhours::SetWindows(std::vector<SoundWindow>());
            return true;

        case IDM_SOUNDHOURS_ALLDAY:
            cfg.soundHoursTouched = true;
            soundhours::SetWindows(std::vector<SoundWindow>{soundhours::kAllDay});
            return true;

        case IDM_SOUNDHOURS_CUSTOM: {
            std::wstring from;
            std::wstring to;
            if (!dialogs::TwoFieldInput(owner, L"Add Sound Hours", L"From", L"8:00 AM", L"To",
                                        L"1:00 PM", &from, &to)) {
                return true;
            }
            const int start = soundhours::ParseTimeOfDay(from);
            const int end = soundhours::ParseTimeOfDay(to);
            if (start < 0 || end < 0) {
                dialogs::Error(owner, L"Add Sound Hours", L"Couldn't read those times");
                return true;
            }
            if (start == end) {
                dialogs::Error(owner, L"Add Sound Hours", L"That window has no length");
                return true;
            }
            std::vector<SoundWindow> list = cfg.soundHours;
            SoundWindow added;
            added.startMinutes = start;
            added.endMinutes = end;
            list.push_back(added);
            cfg.soundHoursTouched = true;
            soundhours::SetWindows(std::move(list));
            return true;
        }

        // ---- alerts --------------------------------------------------------
        case IDM_ALERTS_OFF:
            alerts::SetLeads(std::vector<int>());
            alerts::ResetFiredMap();
            return true;

        case IDM_ALERTS_CUSTOM_LEAD: {
            double minutes = 3;
            if (!dialogs::NumberInput(owner, L"Add a Lead Time", L"Minutes before a block starts",
                                      0.25, 120, false, &minutes)) {
                return true;
            }
            std::vector<int> leads = cfg.alertLeads;
            leads.push_back(static_cast<int>(minutes * 60 + 0.5));
            alerts::SetLeads(std::move(leads));
            alerts::ResetFiredMap();
            soundhours::ArmIfUntouched();
            return true;
        }

        case IDM_ALERTS_SOUND_OFF:
            cfg.alertSound = false;
            cfg.Save();
            return true;

        case IDM_ALERTS_SOUND_IMPORT:
            if (alerts::ImportSound(owner)) {
                // Importing one and then having to pick it from a list would be
                // two steps for one intention.
                const std::vector<std::wstring> sounds = alerts::AvailableSounds();
                if (!sounds.empty()) ChooseSound(sounds.back());
            }
            return true;

        case IDM_ALERTS_VOICE_OFF:
            cfg.alertSpeech = false;
            cfg.Save();
            return true;

        case IDM_ALERTS_VOICE_MANAGE:
            ::ShellExecuteW(owner, L"open", L"ms-settings:speech", nullptr, nullptr,
                            SW_SHOWNORMAL);
            return true;

        case IDM_ALERTS_CATS_ALL:
            // Stored empty rather than as the full list, so a category added by
            // a later import is included rather than silently left out.
            cfg.alertCategories.clear();
            cfg.Save();
            return true;

        case IDM_ALERTS_TEST:
            alerts::TestNow();
            return true;

        // ---- chime ---------------------------------------------------------
        case IDM_CHIME_OFF:
            cfg.chimeMode = ChimeMode::Off;
            cfg.Save();
            return true;

        case IDM_CHIME_HOURLY:
            cfg.chimeMode = ChimeMode::Hourly;
            cfg.Save();
            soundhours::ArmIfUntouched();
            return true;

        case IDM_CHIME_QUARTERLY:
            cfg.chimeMode = ChimeMode::Quarterly;
            cfg.Save();
            soundhours::ArmIfUntouched();
            return true;

        case IDM_CHIME_STRIKE_HOUR:
            cfg.chimeStrikesHour = !cfg.chimeStrikesHour;
            cfg.Save();
            return true;

        case IDM_CHIME_VOL_CUSTOM: {
            double volume = 60;
            // One to a hundred, never zero: silence is what Off and Sound Hours
            // are for, and a chime set to nothing is a setting that looks broken.
            if (!dialogs::NumberInput(owner, L"Custom Chime Volume", L"Volume, 1 to 100", 1, 100,
                                      true, &volume)) {
                return true;
            }
            const int whole = static_cast<int>(volume + 0.5);
            if (std::find(cfg.chimeCustomVolumes.begin(), cfg.chimeCustomVolumes.end(), whole) ==
                cfg.chimeCustomVolumes.end()) {
                cfg.chimeCustomVolumes.push_back(whole);
            }
            cfg.chimeVolume = static_cast<float>(whole) / 100.0f;
            cfg.Save();
            return true;
        }

        case IDM_CHIME_HEAR_PAST:
        case IDM_CHIME_HEAR_HALF:
        case IDM_CHIME_HEAR_TO:
        case IDM_CHIME_HEAR_HOUR: {
            const TimeZone::Parts p = app.Zone().Break(Clock::Now());
            const int hour12 = (p.hour % 12 == 0) ? 12 : p.hour % 12;
            const westminster::Quarter quarter =
                id == IDM_CHIME_HEAR_PAST   ? westminster::Quarter::Past
                : id == IDM_CHIME_HEAR_HALF ? westminster::Quarter::Half
                : id == IDM_CHIME_HEAR_TO   ? westminster::Quarter::To
                                            : westminster::Quarter::Hour;
            westminster::Ring(quarter, hour12);
            return true;
        }

        case IDM_CHIME_STOP:
            westminster::Stop();
            return true;

        // ---- run at startup ------------------------------------------------
        case IDM_STARTUP_OFF:
            SetStartup(owner, false, cfg.startupDelay);
            return true;

        case IDM_STARTUP_ON:
            SetStartup(owner, true, cfg.startupDelay);
            return true;

        default:
            break;
    }

    // ---- the dynamic ranges ------------------------------------------------
    //
    // Tested from the highest base downwards, so an id falls into exactly one
    // range and a list that has outgrown its allocation fails loudly here
    // rather than quietly running someone else's command.

    if (id >= IDM_DAYROW_BASE && id < IDM_RANGE_LAST) {
        return true;   // a listing, not a command
    }

    // Upper bound is the startup-delay range, not the flash range: the delays
    // sit between the two and would otherwise be read as font removals.
    if (id >= IDM_FONTSIZE_DEL_BASE && id < IDM_STARTUP_DELAY_BASE) {
        const size_t index = id - IDM_FONTSIZE_DEL_BASE;
        if (index >= cfg.fontSizeCustoms.size()) return false;
        const double removed = cfg.fontSizeCustoms[index];
        cfg.fontSizeCustoms.erase(cfg.fontSizeCustoms.begin() +
                                  static_cast<std::ptrdiff_t>(index));
        // Removing the size currently in use falls back to the default rather
        // than leaving the strip at a size that is no longer on the menu.
        if (std::fabs(cfg.titleFontSize - removed) < 0.01) cfg.titleFontSize = 0;
        cfg.Save();
        AfterFontChange(app);
        return true;
    }

    if (id >= IDM_FONTSIZE_CUST_BASE && id < IDM_FONTSIZE_DEL_BASE) {
        const size_t index = id - IDM_FONTSIZE_CUST_BASE;
        if (index >= cfg.fontSizeCustoms.size()) return false;
        cfg.titleFontSize = cfg.fontSizeCustoms[index];
        cfg.Save();
        AfterFontChange(app);
        return true;
    }

    if (id >= IDM_FONTSIZE_BASE && id < IDM_FONTSIZE_CUST_BASE) {
        const size_t index = id - IDM_FONTSIZE_BASE;
        if (index >= std::size(kFontSizeChoices)) return false;
        cfg.titleFontSize = kFontSizeChoices[index];
        cfg.Save();
        AfterFontChange(app);
        return true;
    }

    if (id >= IDM_FLASH_BASE && id < IDM_MONITOR_BASE) {
        const size_t index = id - IDM_FLASH_BASE;
        if (index >= std::size(kFlashChoices)) return false;
        cfg.endingFlashSeconds = static_cast<double>(kFlashChoices[index]);
        cfg.Save();
        AfterAppearanceChange(app);
        return true;
    }

    if (id >= IDM_MONITOR_BASE && id < IDM_DAYROW_BASE) {
        const size_t index = id - IDM_MONITOR_BASE;
        const std::vector<TaskbarInfo> bars = EnumerateTaskbars();
        if (index >= bars.size()) return false;

        cfg.monitorDevice = bars[index].monitorDevice;
        // A dragged offset is measured along the taskbar it was dragged on, so
        // it is meaningless on a different one. Cleared rather than carried
        // across, which would park the strip at an arbitrary point.
        cfg.widgetOffsetFromRight = -1;
        cfg.Save();
        app.RelocateToTaskbar();
        return true;
    }

    if (id >= IDM_STARTUP_DELAY_BASE && id < IDM_FLASH_BASE) {
        const size_t index = id - IDM_STARTUP_DELAY_BASE;
        if (index >= std::size(kStartupDelays)) return false;
        cfg.startupDelay = kStartupDelays[index];
        cfg.Save();
        // Re-registering only matters while the task exists; changing the delay
        // with startup off is just a stored preference.
        if (autostart::IsEnabled()) SetStartup(owner, true, cfg.startupDelay);
        return true;
    }

    if (id >= IDM_CHIMEVOL_DEL_BASE && id < IDM_FONTSIZE_BASE) {
        const std::vector<int> volumes = VolumeList();
        const size_t index = id - IDM_CHIMEVOL_DEL_BASE;
        if (index >= volumes.size()) return false;
        const int removed = volumes[index];

        cfg.chimeCustomVolumes.erase(
            std::remove(cfg.chimeCustomVolumes.begin(), cfg.chimeCustomVolumes.end(), removed),
            cfg.chimeCustomVolumes.end());

        // Removing the volume currently in use falls back to the nearest
        // preset, rather than leaving the chime set to a value with no row.
        if (std::fabs(static_cast<double>(cfg.chimeVolume) * 100 - removed) < 0.5) {
            int best = kVolumePresets[0];
            for (int v : kVolumePresets) {
                if (std::abs(v - removed) < std::abs(best - removed)) best = v;
            }
            cfg.chimeVolume = static_cast<float>(best) / 100.0f;
        }
        cfg.Save();
        return true;
    }

    if (id >= IDM_CHIMEVOL_BASE && id < IDM_CHIMEVOL_DEL_BASE) {
        const std::vector<int> volumes = VolumeList();
        const size_t index = id - IDM_CHIMEVOL_BASE;
        if (index >= volumes.size()) return false;
        cfg.chimeVolume = static_cast<float>(volumes[index]) / 100.0f;
        cfg.Save();
        return true;
    }

    if (id >= IDM_CATEGORY_BASE && id < IDM_CHIMEVOL_BASE) {
        const std::vector<std::wstring> categories = CategoryList();
        const size_t index = id - IDM_CATEGORY_BASE;
        if (index >= categories.size()) return false;
        alerts::ToggleCategory(categories[index]);
        return true;
    }

    if (id >= IDM_VOICE_BASE && id < IDM_CATEGORY_BASE) {
        const std::vector<alerts::Voice> voices = alerts::AvailableVoices();
        const size_t index = id - IDM_VOICE_BASE;
        if (index >= voices.size()) return false;
        ChooseVoice(voices[index].id);
        return true;
    }

    if (id >= IDM_SOUNDNAME_BASE && id < IDM_VOICE_BASE) {
        const std::vector<std::wstring> sounds = alerts::AvailableSounds();
        const size_t index = id - IDM_SOUNDNAME_BASE;
        if (index >= sounds.size()) return false;
        ChooseSound(sounds[index]);
        return true;
    }

    if (id >= IDM_LEAD_BASE && id < IDM_SOUNDNAME_BASE) {
        const std::vector<int> leads = LeadList();
        const size_t index = id - IDM_LEAD_BASE;
        if (index >= leads.size()) return false;

        const int seconds = leads[index];
        std::vector<int> chosen = cfg.alertLeads;
        const auto it = std::find(chosen.begin(), chosen.end(), seconds);
        if (it != chosen.end()) {
            chosen.erase(it);
        } else {
            chosen.push_back(seconds);
        }
        alerts::SetLeads(std::move(chosen));
        alerts::ResetFiredMap();
        soundhours::ArmIfUntouched();
        return true;
    }

    if (id >= IDM_SOUNDWINDOW_DEL_BASE && id < IDM_LEAD_BASE) {
        const std::vector<SoundWindow> list = SoundWindowList();
        const size_t index = id - IDM_SOUNDWINDOW_DEL_BASE;
        if (index >= list.size()) return false;
        soundhours::Remove(list[index]);
        return true;
    }

    if (id >= IDM_SOUNDWINDOW_BASE && id < IDM_SOUNDWINDOW_DEL_BASE) {
        const std::vector<SoundWindow> list = SoundWindowList();
        const size_t index = id - IDM_SOUNDWINDOW_BASE;
        if (index >= list.size()) return false;
        soundhours::Toggle(list[index]);
        return true;
    }

    if (id >= IDM_PROFILE_REMOVE_BASE && id < IDM_SOUNDWINDOW_BASE) {
        const size_t index = id - IDM_PROFILE_REMOVE_BASE;
        if (index >= cfg.profiles.size()) return false;
        RemoveProfileFlow(app, owner, cfg.profiles[index]);
        return true;
    }

    if (id >= IDM_PROFILE_RENAME_BASE && id < IDM_PROFILE_REMOVE_BASE) {
        const size_t index = id - IDM_PROFILE_RENAME_BASE;
        if (index >= cfg.profiles.size()) return false;
        std::wstring name = cfg.profiles[index].name;
        if (!dialogs::TextInput(owner, L"Rename Calendar", L"Name", L"Work", &name)) return true;
        name = Trim(name);
        if (name.empty()) return true;
        cfg.RenameProfile(cfg.profiles[index].name, name);
        app.InvalidateStrip();
        return true;
    }

    if (id >= IDM_PROFILE_BASE && id < IDM_PROFILE_RENAME_BASE) {
        const size_t index = id - IDM_PROFILE_BASE;
        if (index >= cfg.profiles.size()) return false;
        cfg.ActivateProfile(cfg.profiles[index].name);
        app.ReloadAfterSourceChange();
        return true;
    }

    if (id >= IDM_LABELWIDTH_BASE && id < IDM_PROFILE_BASE) {
        const size_t index = id - IDM_LABELWIDTH_BASE;
        if (index >= std::size(kLabelWidths)) return false;
        cfg.maxLabelWidth = kLabelWidths[index];
        AfterAppearanceChange(app);
        return true;
    }

    if (id >= IDM_WIDTH_BASE && id < IDM_LABELWIDTH_BASE) {
        const int index = static_cast<int>(id - IDM_WIDTH_BASE);
        if (index >= kWidthCount) return false;
        cfg.timelineWidth = WidthOption(index);
        AfterAppearanceChange(app);
        return true;
    }

    if (id >= IDM_TIMERANGE_BASE && id < IDM_WIDTH_BASE) {
        const size_t index = id - IDM_TIMERANGE_BASE;
        if (index >= std::size(kTimeRanges)) return false;
        cfg.windowMinutes = kTimeRanges[index];
        AfterAppearanceChange(app);
        return true;
    }

    return false;
}

}  // namespace menu
}  // namespace rc
