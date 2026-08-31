// app.h — the widget window and everything hanging off it.
//
// One window, embedded in the taskbar, repainting once a second. That tick also
// re-measures the widget, checks the alert schedule and checks the chime, so
// there is exactly one timer in the app and nothing else is scheduled. Idle
// cost is meant to be indistinguishable from zero: a redraw a second, a fetch
// every five minutes.
//
// The feed is re-read every five minutes, at launch, on wake, and whenever the
// source changes. A failed refresh keeps the last good day for thirty minutes
// before clearing it -- by then it really is unknown, but until then showing
// yesterday's plan beats showing nothing.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "daylist.h"
#include "ics.h"
#include "raii.h"
#include "taskbar.h"
#include "timeline.h"

namespace rc {

// Posted to the widget window when a fetch finishes; lParam is a FetchResult*
// the window takes ownership of.
#define WM_APP_FETCH_DONE (WM_APP + 1)
#define WM_APP_TRAY_ICON  (WM_APP + 2)

class App {
public:
    static App& Get();

    bool Initialize(HINSTANCE instance);
    int Run();
    void Quit();

    HWND Window() const { return hwnd_; }
    Timeline& GetTimeline() { return timeline_; }
    const TimeZone& Zone() const { return zone_; }

    // ---- data ---------------------------------------------------------
    void Refresh();                       // start a fetch now
    void ReloadAfterSourceChange();       // switch source, clear, refetch
    void ApplyKeywordRules();             // recolour without refetching

    const std::vector<CalEvent>& AllEvents() const { return allEvents_; }
    std::vector<CalEvent> CycleEvents() const;
    std::wstring SourceName() const;
    Seconds LastFetch() const { return lastFetch_; }
    const std::wstring& ErrorMessage() const { return error_; }

    // ---- widget -------------------------------------------------------
    void RelayoutNow();                   // re-measure and reposition

    // Moves the strip to the taskbar named by the current monitor preference.
    // Re-parents, re-establishes the host, and rebuilds the fonts, because two
    // displays can be at different scaling factors and a strip that keeps the
    // old monitor's font metrics is either clipped or half the size it should
    // be.
    void RelocateToTaskbar();
    void InvalidateStrip();
    void BeginMoveWidget();               // arms drag-to-reposition
    void ResetWidgetPosition();

    // ---- menu ---------------------------------------------------------
    void ShowMenu(POINT screenPt);

private:
    App() = default;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void OnTick();
    void OnFetchDone(struct FetchResult* result);
    void OnTaskbarCreated();
    void OnThemeChanged();
    void OnWake();

    void RebuildZone();
    void LoadDemo();
    void ShowFailure(const std::wstring& message);

    void EnsureTrayIcon();
    void UpdateTrayTooltip(const std::wstring& text);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HostMode hostMode_ = HostMode::Floating;
    TaskbarInfo taskbar_{};
    UINT taskbarCreatedMsg_ = 0;

    Timeline timeline_;
    TimeZone zone_;

    std::vector<CalEvent> allEvents_;   // alerts see all of these
    std::wstring error_;
    Seconds lastFetch_ = 0;
    Seconds failingSince_ = 0;
    Seconds lastRefetch_ = 0;
    unsigned long fetchToken_ = 0;

    int widgetWidth_ = 0;
    int widgetThickness_ = 0;
    bool draggingArmed_ = false;
    bool dragging_ = false;
    int dragGrabOffset_ = 0;

    bool trayIconAdded_ = false;
    Seconds lastTooltipWrite_ = 0;

    // Events are dropped only once a failure has persisted this long. Until
    // then the strip shows the error and the last good day underneath it.
    static constexpr Seconds kFailureGrace = 1800;
    static constexpr Seconds kRefetchInterval = 300;
};

}  // namespace rc
