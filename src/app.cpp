// app.cpp — see app.h for the shape of the thing.
//
// The whole app is one window and one timer. That is not minimalism for its own
// sake: a widget that lives in the taskbar for months at a time is judged on
// what it costs while nothing is happening, and every additional timer, thread
// or window is a cost that is paid whether or not anyone is looking at it. So
// the one-second tick does the redraw, the re-measure, the taskbar check, the
// alert check and the chime check, and the only other work in the process is a
// fetch every five minutes on a thread that exits when it is done.
//
// The ordering rules that are easy to get wrong, collected here rather than
// scattered as one-line comments:
//
//   * Calendar arithmetic uses Clock::Now(), which carries the Debug Time
//     offset. Freshness -- how old the fetched data is -- uses RealNow(),
//     because how stale the feed is has nothing to do with a simulated clock.
//   * A fetch result is dropped unless its token is still the current
//     generation, so switching calendars mid-request cannot resurrect the old
//     one.
//   * A failure keeps the last good day for thirty minutes before clearing it.

#include "app.h"

#include <windows.h>
#include <windowsx.h>

#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <memory>

#include "alerts.h"
#include "autostart.h"
#include "calsource.h"
#include "common.h"
#include "demodata.h"
#include "fetch.h"
#include "ics.h"
#include "keywords.h"
#include "menu.h"
#include "resource.h"
#include "settings.h"
#include "taskbar.h"
#include "westminster.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace rc {
namespace {

const wchar_t* const kWindowClass = L"RollingCalendarWidget";

constexpr UINT_PTR kTickTimerId = 1;
constexpr UINT kTickIntervalMs = 1000;
constexpr UINT kTrayIconId = 1;

// How often the taskbar is re-queried, in ticks. The shell does not announce a
// move, a resize or a change of orientation to anybody who is not a registered
// appbar, so the only way to notice is to look.
constexpr int kTaskbarPollTicks = 2;

// How often the tray tooltip may be rewritten. Shell_NotifyIcon is a
// cross-process call into Explorer, and this text is only ever visible while
// the pointer is resting on the icon.
constexpr Seconds kTooltipInterval = 5;

int RectWidth(const RECT& r) { return static_cast<int>(r.right - r.left); }
int RectHeight(const RECT& r) { return static_cast<int>(r.bottom - r.top); }

bool IsHorizontalEdge(TaskbarEdge edge) {
    return edge == TaskbarEdge::Bottom || edge == TaskbarEdge::Top;
}

int BarLength(const TaskbarInfo& info) {
    return IsHorizontalEdge(info.edge) ? RectWidth(info.bounds) : RectHeight(info.bounds);
}

// Embedded and floating want opposite window styles. taskbar.cpp deliberately
// touches only the extended style, because that is all the re-parenting itself
// needs; the WS_CHILD / WS_POPUP swap belongs to whoever owns the window, which
// is here. A child of the desktop is not affected by HWND_TOPMOST, so a
// fallback that skipped this would end up behind the taskbar it was meant to
// sit on top of.
void ApplyHostStyle(HWND hwnd, HostMode mode) {
    if (!hwnd) return;

    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (mode == HostMode::Embedded) {
        style |= static_cast<LONG_PTR>(WS_CHILD);
        style &= ~static_cast<LONG_PTR>(WS_POPUP);
    } else {
        style |= static_cast<LONG_PTR>(WS_POPUP);
        style &= ~static_cast<LONG_PTR>(WS_CHILD);
    }
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);
}

// The link the display time zone is read from: the active profile's, or the
// bare URL when there are no profiles.
std::wstring ActiveLink() {
    const Settings& cfg = Cfg();
    for (const CalendarProfile& profile : cfg.profiles) {
        if (profile.name == cfg.activeProfile) return profile.link;
    }
    return cfg.calendarUrl;
}

// The previous tooltip text, kept here rather than on App because the header is
// a fixed contract and App is a singleton, so a file-scope copy is the same
// object either way.
std::wstring g_lastTooltip;

}  // namespace

// ---------------------------------------------------------------- lifecycle

App& App::Get() {
    static App app;
    return app;
}

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;

    Cfg().Load();

    // "Cleared stays cleared": the sample is seeded once, guarded by a flag,
    // rather than whenever the rule list happens to be empty. Otherwise Clear
    // Keyword Colors would undo itself on the next launch.
    if (!Cfg().keywordRulesSeeded) {
        keywords::SetRules(keywords::SampleRules(), L"the built-in sample");
        Cfg().keywordRulesSeeded = true;
        Cfg().SaveKeywordRules(keywords::Rules());
        Cfg().keywordRulesSource = keywords::SourceName();
        Cfg().Save();
    } else {
        keywords::SetRules(Cfg().LoadKeywordRules(), Cfg().keywordRulesSource);
    }

    // The pre-profiles key. Adopting it as a profile is what makes the upgrade
    // invisible: the calendar the user already had keeps working and now has a
    // name, and the legacy key is never written back.
    if (!Cfg().calendarUrl.empty() && Cfg().profiles.empty()) {
        Cfg().AddProfile(calsource::Label(Cfg().calendarUrl), Cfg().calendarUrl);
        Cfg().Save();
    }

    RebuildZone();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND is answered; the strip fills its own client area
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // The taskbar is queried before the window is created so the window can be
    // born as a child of it. A WS_CHILD window needs a parent at creation, and
    // creating it under the desktop only to re-parent it a line later would
    // make it flicker into view in the top-left corner first.
    taskbar_ = QueryTaskbar();
    const HWND parent = taskbar_.valid ? taskbar_.window : ::GetDesktopWindow();

    // WS_EX_TOOLWINDOW keeps it out of Alt-Tab and off the taskbar it is sitting
    // in; WS_EX_NOACTIVATE stops a click stealing focus from whatever the user
    // was actually typing into. Deliberately *not* WS_EX_LAYERED: a layered
    // window cannot be a child window, which is why timeline.cpp approximates
    // the taskbar background rather than letting it show through.
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kWindowClass, kDisplayName,
                              WS_CHILD | WS_VISIBLE,
                              0, 0, 1, 1,
                              parent, nullptr, instance_, nullptr);
    if (!hwnd_) return false;

    hostMode_ = EmbedInTaskbar(hwnd_, taskbar_);
    ApplyHostStyle(hwnd_, hostMode_);

    timeline_.UpdateFonts(DpiForWindow(hwnd_));

    alerts::Init();
    westminster::Init();
    autostart::RepairIfMoved();
    EnsureTrayIcon();

    RelayoutNow();

    ::SetTimer(hwnd_, kTickTimerId, kTickIntervalMs, nullptr);

    // Explorer restarts more often than people think -- a crash, a shell
    // extension update, a theme change on some builds -- and takes every
    // embedded child with it silently.
    taskbarCreatedMsg_ = RegisterTaskbarCreatedMessage();

    Refresh();
    return true;
}

int App::Run() {
    // Zero-initialised because GetMessage returning -1 leaves it untouched, and
    // the exit code is read from it either way.
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    alerts::Shutdown();
    westminster::Shutdown();
    Cfg().Save();
    return static_cast<int>(msg.wParam);
}

void App::Quit() {
    if (hwnd_) ::DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------- data

void App::Refresh() {
    lastRefetch_ = RealNow();

    if (Cfg().demoMode) {
        // Generated in memory, so there is nothing to wait for and no reason to
        // put it on a thread.
        LoadDemo();
        lastFetch_ = RealNow();
        InvalidateStrip();
        return;
    }

    fetchToken_ = StartFetch(Cfg().calendarUrl, hwnd_);
}

void App::ReloadAfterSourceChange() {
    RebuildZone();

    // The old events belong to the old calendar. Keeping them across a switch
    // would show one calendar's blocks under another calendar's name for as
    // long as the fetch took.
    allEvents_.clear();
    timeline_.SetEvents({});
    error_.clear();
    timeline_.SetError(L"");
    failingSince_ = 0;

    Refresh();
    InvalidateStrip();
}

void App::ApplyKeywordRules() {
    keywords::Apply(allEvents_);
    timeline_.SetEvents(allEvents_);
    InvalidateStrip();
}

std::vector<CalEvent> App::CycleEvents() const {
    return daylist::CycleEvents(allEvents_, Clock::Now(), zone_, Cfg().dayAnchorKeyword);
}

std::wstring App::SourceName() const {
    return Cfg().SourceDisplayName();
}

void App::LoadDemo() {
    allEvents_ = demodata::Events(Clock::Now(), zone_);

    // Through the same rule set a real feed goes through, so clearing the
    // keyword colours turns the demo grey exactly as it would turn a real
    // calendar grey.
    keywords::Apply(allEvents_);
    timeline_.SetEvents(allEvents_);

    error_.clear();
    timeline_.SetError(L"");
    failingSince_ = 0;
}

void App::OnFetchDone(FetchResult* result) {
    std::unique_ptr<FetchResult> owned(result);
    if (!owned) return;

    // Stale. The user changed calendars, or hit Refresh Now twice, while this
    // one was still in the air.
    if (owned->token != CurrentFetchToken()) return;

    if (owned->status != FetchStatus::Ok) {
        ShowFailure(owned->message);
        return;
    }

    std::vector<CalEvent> events =
        ics::Parse(owned->body, Clock::Now(), zone_, ics::DefaultDayOffsets());
    keywords::Apply(events);

    allEvents_ = std::move(events);

    // The Timeline does its own filtering to the drawing window, so it is given
    // the whole set rather than a second filtered copy to keep in step.
    timeline_.SetEvents(allEvents_);

    error_.clear();
    timeline_.SetError(L"");
    failingSince_ = 0;
    lastFetch_ = RealNow();

    InvalidateStrip();
}

void App::ShowFailure(const std::wstring& message) {
    if (failingSince_ == 0) failingSince_ = RealNow();

    error_ = message;
    timeline_.SetError(message);

    // A feed that has been unreachable for half an hour is genuinely unknown,
    // so the day goes. Until then the last good day survives underneath the
    // error: at ten past nine, yesterday's plan for ten past nine is still a
    // better answer than a blank strip. If there was nothing cached in the
    // first place there is nothing to preserve and the error stands alone.
    const bool nothingCached = allEvents_.empty();
    if (nothingCached || RealNow() - failingSince_ > kFailureGrace) {
        allEvents_.clear();
        timeline_.SetEvents({});
    }

    InvalidateStrip();
}

void App::RebuildZone() {
    zone_ = TimeZone();

    // Demo mode always reads in the machine's own zone. Its blocks are built
    // against local midnight, so displaying them in somebody else's zone would
    // put "Sleep" in the middle of the afternoon.
    if (Cfg().demoMode) return;

    const std::wstring link = ActiveLink();
    if (link.empty()) return;

    // A calendar published with ctz= is meant to be read in that zone, not the
    // reader's -- that is the whole point of the parameter.
    const std::optional<std::wstring> iana = calsource::TimeZoneOf(link);
    if (iana) zone_.SetIana(*iana);
}

// -------------------------------------------------------------------- widget

void App::RelayoutNow() {
    if (!hwnd_) return;

    widgetWidth_ = std::max(1, timeline_.Measure(Clock::Now()));
    widgetThickness_ = std::max(1, UsableThickness(taskbar_));

    PositionWidget(hwnd_, taskbar_, hostMode_,
                   AutoOffsetAlong(taskbar_, widgetWidth_, Cfg().widgetOffsetFromRight),
                   widgetWidth_, widgetThickness_);
    InvalidateStrip();
}

void App::InvalidateStrip() {
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::BeginMoveWidget() {
    draggingArmed_ = true;
    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
}

void App::ResetWidgetPosition() {
    Cfg().widgetOffsetFromRight = -1;
    Cfg().Save();
    RelayoutNow();
}

// ---------------------------------------------------------------------- tick

void App::OnTick() {
    const Seconds now = Clock::Now();

    // A resize forces the taskbar to relayout everything in it, so a widget
    // that oscillated by a pixel would make the whole bar twitch once a second.
    // One physical pixel of hysteresis costs nothing and removes the effect.
    const int wanted = std::max(1, timeline_.Measure(now));
    const int thickness = std::max(1, UsableThickness(taskbar_));
    if (std::abs(wanted - widgetWidth_) > 1 || thickness != widgetThickness_) {
        widgetWidth_ = wanted;
        widgetThickness_ = thickness;
        if (!dragging_) {
            PositionWidget(hwnd_, taskbar_, hostMode_,
                           AutoOffsetAlong(taskbar_, widgetWidth_, Cfg().widgetOffsetFromRight),
                           widgetWidth_, widgetThickness_);
        }
    }

    // The shell never says that the taskbar has moved, changed edge or changed
    // height, so it is checked by looking. One FindWindow and one GetWindowRect
    // every two seconds is far below anything measurable, and it is done inside
    // the tick that is already running rather than on a second timer.
    static int taskbarPoll = 0;
    if (++taskbarPoll >= kTaskbarPollTicks) {
        taskbarPoll = 0;
        const TaskbarInfo fresh = QueryTaskbar();
        if (fresh.valid &&
            (fresh.window != taskbar_.window || !::EqualRect(&fresh.bounds, &taskbar_.bounds) ||
             fresh.edge != taskbar_.edge || fresh.dpi != taskbar_.dpi ||
             !::EqualRect(&fresh.notifyBounds, &taskbar_.notifyBounds))) {
            const bool newWindow = (fresh.window != taskbar_.window);
            const int oldDpi = taskbar_.dpi;
            taskbar_ = fresh;
            if (newWindow) {
                hostMode_ = EmbedInTaskbar(hwnd_, taskbar_);
                ApplyHostStyle(hwnd_, hostMode_);
            }
            if (taskbar_.dpi != oldDpi) timeline_.UpdateFonts(DpiForWindow(hwnd_));
            if (!dragging_) RelayoutNow();
        }
    }

    InvalidateStrip();

    // Alerts see every loaded event, not the dropdown's cycle: a block outside
    // the sleep-to-sleep window is still a block that is about to start.
    alerts::Tick(allEvents_, now, zone_);
    westminster::Tick(now, zone_);

    if (RealNow() - lastRefetch_ >= kRefetchInterval) Refresh();

    if (RealNow() - lastTooltipWrite_ >= kTooltipInterval) {
        UpdateTrayTooltip(error_.empty() ? timeline_.TooltipText(now) : error_);
    }
}

// ------------------------------------------------------------------ recovery

void App::OnTaskbarCreated() {
    // Explorer has restarted. The old taskbar window is gone, this window is
    // parentless, and the tray icon it used to own no longer exists.
    taskbar_ = QueryTaskbar();
    hostMode_ = EmbedInTaskbar(hwnd_, taskbar_);
    ApplyHostStyle(hwnd_, hostMode_);

    trayIconAdded_ = false;
    EnsureTrayIcon();

    timeline_.UpdateFonts(DpiForWindow(hwnd_));
    RelayoutNow();
}

void App::OnThemeChanged() {
    // Both the fonts and every cached label depend on the theme: the label
    // cache is keyed partly on dark mode, and the shell can change its menu
    // font at the same time it changes colours.
    timeline_.UpdateFonts(DpiForWindow(hwnd_));
    timeline_.InvalidateLabelCache();
    InvalidateStrip();
}

void App::OnWake() {
    // The machine has been asleep, so the feed is stale by however long that
    // was, and the audio device list may have been rebuilt under us while the
    // endpoints were powered down.
    Refresh();
    alerts::RefreshVoices();
    alerts::RefreshSounds();
    westminster::OnDeviceChanged();
}

// ----------------------------------------------------------------- tray icon

void App::EnsureTrayIcon() {
    if (trayIconAdded_ || !hwnd_) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon = ::LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    if (!nid.hIcon) nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::wcsncpy_s(nid.szTip, kDisplayName, _TRUNCATE);

    // The icon is the escape hatch. The strip lives among the taskbar's own
    // children and can end up behind a crowded row of pinned apps, or fail to
    // embed at all on a locked-down machine; an icon in the notification area
    // is a documented, guaranteed way to reach the menu when that happens.
    trayIconAdded_ = (::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
}

void App::UpdateTrayTooltip(const std::wstring& text) {
    if (!trayIconAdded_ || !hwnd_) return;
    if (text == g_lastTooltip) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    ::wcsncpy_s(nid.szTip, text.c_str(), _TRUNCATE);

    if (::Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        g_lastTooltip = text;
        lastTooltipWrite_ = RealNow();
    }
}

// ---------------------------------------------------------------------- menu

void App::ShowMenu(POINT screenPt) {
    if (!hwnd_) return;

    // Built once and held for the life of the popup: menu.h says the rows are
    // retained for owner-drawing and must outlive the HMENU, and `rows` is
    // declared first so it is destroyed last.
    const std::vector<DayRow> rows = daylist::BuildRows(CycleEvents(), Clock::Now(), zone_);

    UniqueMenu popup = menu::Build(*this, rows);
    if (!popup) return;

    // Before TrackPopupMenuEx, not after. Without it the menu only appears when
    // this process happens to hold foreground rights already, which makes it
    // look random: the first click does nothing, the second works, because the
    // first click was what granted the rights.
    ::SetForegroundWindow(hwnd_);

    const UINT id = static_cast<UINT>(::TrackPopupMenuEx(popup.get(),
                                                         TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                                         screenPt.x, screenPt.y, hwnd_, nullptr));
    popup.reset();

    if (id != 0) menu::Invoke(*this, hwnd_, id);
    menu::ReleaseItemData();

    // The long-standing TrackPopupMenu workaround: without a message posted
    // afterwards the menu can refuse to close on the next click outside it.
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
}

// ------------------------------------------------------------ message pump

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = App::Get();

    // WM_NCCREATE and WM_CREATE arrive before CreateWindowExW has returned, so
    // the handle has to come from here rather than from the member.
    if (!app.hwnd_) app.hwnd_ = hwnd;
    if (hwnd != app.hwnd_) return ::DefWindowProcW(hwnd, msg, wp, lp);

    return app.Handle(msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        OnTaskbarCreated();
        return 0;
    }

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = ::BeginPaint(hwnd_, &ps);
            if (dc) {
                RECT client{};
                ::GetClientRect(hwnd_, &client);

                // Measure before Paint: the gutters are composed by Measure and
                // cached, and Paint needs the widths it worked out.
                timeline_.Measure(Clock::Now());
                timeline_.Paint(dc, client);
                ::EndPaint(hwnd_, &ps);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            // The strip paints every pixel of its client area from a back
            // buffer. Letting the class brush run first would only add a flash
            // of the wrong colour.
            return 1;

        case WM_TIMER:
            if (wp == kTickTimerId) OnTick();
            return 0;

        case WM_APP_FETCH_DONE:
            OnFetchDone(reinterpret_cast<FetchResult*>(lp));
            return 0;

        case WM_APP_TRAY_ICON:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ShowMenu(pt);
            }
            return 0;

        case WM_SETCURSOR:
            if (draggingArmed_ || dragging_) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN:
            if (draggingArmed_) {
                dragging_ = true;
                ::SetCapture(hwnd_);

                RECT self{};
                ::GetWindowRect(hwnd_, &self);
                const bool horizontal = IsHorizontalEdge(taskbar_.edge);
                POINT pt{};
                ::GetCursorPos(&pt);
                dragGrabOffset_ = horizontal ? static_cast<int>(pt.x - self.left)
                                             : static_cast<int>(pt.y - self.top);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (dragging_) {
                const bool horizontal = IsHorizontalEdge(taskbar_.edge);
                POINT pt{};
                ::GetCursorPos(&pt);

                const int along = horizontal
                                      ? static_cast<int>(pt.x - taskbar_.bounds.left)
                                      : static_cast<int>(pt.y - taskbar_.bounds.top);
                const int limit = std::max(0, BarLength(taskbar_) - widgetWidth_);
                const int offset = std::max(0, std::min(along - dragGrabOffset_, limit));

                PositionWidget(hwnd_, taskbar_, hostMode_, offset, widgetWidth_, widgetThickness_);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                draggingArmed_ = false;
                ::ReleaseCapture();

                RECT self{};
                ::GetWindowRect(hwnd_, &self);
                const bool horizontal = IsHorizontalEdge(taskbar_.edge);
                const int start = horizontal ? static_cast<int>(self.left - taskbar_.bounds.left)
                                             : static_cast<int>(self.top - taskbar_.bounds.top);

                // Stored as a distance from the trailing edge rather than the
                // leading one. A dragged position therefore anchors to the
                // right, so a tray that grows an icon creeps the widget along
                // with it; automatic placement, which measures from the tray
                // itself, does not have that problem, which is why -1 stays the
                // default.
                Cfg().widgetOffsetFromRight =
                    std::max(0, BarLength(taskbar_) - start - widgetWidth_);
                Cfg().Save();
                return 0;
            }
            {
                // A plain click on the strip opens the menu, which is what the
                // macOS status item does; there is nothing else to click.
                POINT pt{};
                ::GetCursorPos(&pt);
                ShowMenu(pt);
            }
            return 0;

        case WM_RBUTTONUP: {
            POINT pt{};
            ::GetCursorPos(&pt);
            ShowMenu(pt);
            return 0;
        }

        case WM_MEASUREITEM:
            menu::OnMeasureItem(hwnd_, reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
            return TRUE;

        case WM_DRAWITEM:
            menu::OnDrawItem(hwnd_, reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;

        case WM_COMMAND:
            // TPM_RETURNCMD means the popup normally reports its result to
            // ShowMenu directly; this is here for anything that arrives by
            // another route.
            if (menu::Invoke(*this, hwnd_, LOWORD(wp))) return 0;
            break;

        case WM_SETTINGCHANGE:
            if (lp && ::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                OnThemeChanged();
            }
            return 0;

        case WM_DPICHANGED:
            timeline_.UpdateFonts(DpiForWindow(hwnd_));
            RelayoutNow();
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) OnWake();
            return TRUE;

        case WM_DESTROY: {
            ::KillTimer(hwnd_, kTickTimerId);

            if (trayIconAdded_) {
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd_;
                nid.uID = kTrayIconId;
                ::Shell_NotifyIconW(NIM_DELETE, &nid);
                trayIconAdded_ = false;
            }

            ::PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }

    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace rc
