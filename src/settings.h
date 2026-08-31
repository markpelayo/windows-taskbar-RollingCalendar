// settings.h — everything that survives a relaunch.
//
// One INI file, %APPDATA%\RollingCalendar\settings.ini, cached in memory and
// written through on change, so a redraw never touches the disk.
//
// Restore Defaults deletes the *whole file* rather than a list of keys. A
// setting added in a later version then cannot be left behind by an older
// reset, which is a real bug in every implementation that enumerates keys.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "keywords.h"

namespace rc {

struct CalendarProfile {
    std::wstring name;
    std::wstring link;
};

// A Sound Hours window, in minutes from midnight. `end <= start` wraps past
// midnight; {0, 0} is the explicit "all day" choice, which is not the same as
// having nothing set.
struct SoundWindow {
    int startMinutes = 0;
    int endMinutes = 0;

    bool wrapsMidnight() const { return endMinutes <= startMinutes; }
    bool isAllDay() const { return startMinutes == 0 && endMinutes == 0; }
    bool operator==(const SoundWindow& o) const {
        return startMinutes == o.startMinutes && endMinutes == o.endMinutes;
    }
};

enum class ChimeMode { Off, Hourly, Quarterly };

class Settings {
public:
    static Settings& Get();

    void Load();
    void Save();
    void RestoreAll();          // deletes the file and re-seeds first-run state
    void RestoreStrip();        // the seven appearance keys only

    // ---- strip appearance (all exposed in the menu) --------------------
    double windowMinutes = 120;   // total span, centred on now (+/- 1 hour)
    double timelineWidth = 250;   // logical px; clamped 50..900 on read
    double maxLabelWidth = 360;   // logical px, about 47 characters
    bool showNowName = true;
    bool showNowTimeLeft = true;
    bool showNextName = true;
    bool showNextDuration = true;

    // Text Size, in points. 0 means the shell's own menu-font size, which is
    // what every other thing in the taskbar draws with. Moved out of the hidden
    // settings and onto the menu, so it resets with the rest of the strip's
    // appearance.
    double titleFontSize = 0;
    std::vector<double> fontSizeCustoms;   // sizes the user added, removable

    bool isAppearanceDefault() const;

    // ---- hidden settings (file only, no menu) --------------------------
    double nowLineWidth = 4;
    double urgentSeconds = 120;

    // Ending Soon Flash. How long before a block ends its name starts blinking
    // red, in seconds. 0 is off, which is the default.
    //
    // This is a *warning*, not a piece of geometry, which is why Reset Strip
    // Settings deliberately leaves it alone and only Restore Defaults clears
    // it. A click aimed at the timeline's proportions should not silently turn
    // off the thing telling you a meeting is about to end.
    double endingFlashSeconds = 0;
    bool isFlashing() const { return endingFlashSeconds > 0; }
    COLORREF unmatchedColor = RGB(0x8E, 0x8E, 0x93);
    bool solidBlocks = true;
    double blockGap = 1;
    double blockCornerRadius = 0;   // 0 means a full capsule
    std::wstring dayAnchorKeyword = L"sleep";

    // How the strip is hosted, for when the automatic choice is wrong on a
    // particular machine. 0 auto, 1 plain child, 2 layered child, 3 floating.
    //
    // Automatic means: a layered child inside the taskbar when the shell
    // renders through a composition island (Windows 11), a plain child
    // otherwise. This override exists because the alternative to a one-line
    // edit in an INI file is a rebuild, and the failure it works around is a
    // widget that cannot be seen -- which is not a state to leave anyone stuck
    // in.
    int hostOverride = 0;

    // How far the elapsed part of a block is blended toward white. 0 means use
    // the built-in value, which differs between the light and dark themes.
    // Tunable because how much fading reads as "past" depends entirely on what
    // the wallpaper behind the taskbar looks like.
    double pastFade = 0;

    // Height of the capsule band in logical px. 0 means the built-in value,
    // chosen to match a taskbar icon so the strip sits alongside them rather
    // than filling the bar. Tunable for a taskbar that has been made larger or
    // smaller than the default.
    double blockHeight = 0;

    // Logical px between a gutter and the timeline. 0 means the built-in value.
    double innerGap = 0;

    // ---- calendar source ----------------------------------------------
    std::wstring calendarUrl;
    std::vector<CalendarProfile> profiles;
    std::wstring activeProfile;
    bool demoMode = true;           // on until a calendar is configured

    bool hasCalendarInput() const { return !calendarUrl.empty(); }
    void AddProfile(const std::wstring& name, const std::wstring& link);
    void RenameProfile(const std::wstring& oldName, const std::wstring& newName);
    void RemoveProfile(const std::wstring& name);
    void ActivateProfile(const std::wstring& name);
    std::wstring SourceDisplayName() const;

    // ---- keyword rules -------------------------------------------------
    std::wstring keywordRulesSource = L"the built-in sample";
    bool keywordRulesSeeded = false;
    void SaveKeywordRules(const std::vector<KeywordRule>& rules);
    std::vector<KeywordRule> LoadKeywordRules() const;

    // ---- debug time ----------------------------------------------------
    double debugOffset = 0;   // clamped +/- 3155760000

    // ---- widget placement ----------------------------------------------
    // Stored as a distance from the taskbar's right edge; -1 means automatic
    // placement immediately left of the notification area.
    int widgetOffsetFromRight = -1;

    // Which display's taskbar hosts the strip. The adapter device name; empty
    // means the primary. Kept even when that display is absent, so unplugging
    // a dock does not silently forget the choice.
    std::wstring monitorDevice;

    // ---- sound hours ----------------------------------------------------
    bool soundHoursOn = true;
    std::vector<SoundWindow> soundHours{{690, 270}};   // 11:30 AM - 4:30 AM
    bool soundHoursTouched = false;

    // ---- time block alerts ----------------------------------------------
    std::vector<int> alertLeads;      // seconds, deduped, sorted descending
    bool alertSound = false;
    bool alertSpeech = false;         // mutually exclusive with alertSound
    std::wstring alertSoundName = L"Notify";
    std::wstring alertVoice;          // empty = first installed SAPI voice
    std::vector<std::wstring> alertCategories;   // empty = every category

    bool alertsEnabled() const {
        return !alertLeads.empty() && (alertSound || alertSpeech);
    }

    // ---- westminster chime ----------------------------------------------
    ChimeMode chimeMode = ChimeMode::Off;
    bool chimeStrikesHour = true;
    float chimeVolume = 0.5f;
    std::vector<int> chimeCustomVolumes;

    // ---- run at startup --------------------------------------------------
    bool runAtStartup = false;
    int startupDelay = 20;   // seconds; 5/10/15/20/30/60

    // True when every feature reports itself at its default, which is what
    // greys out Restore Defaults. Asked feature by feature rather than by
    // checking whether the file exists, because a first launch writes keys of
    // its own.
    bool isEverythingDefault() const;

    std::wstring FilePath() const;

private:
    Settings() = default;
    void Reseed();
};

inline Settings& Cfg() { return Settings::Get(); }

}  // namespace rc
