// settings.cpp — everything that survives a relaunch. See settings.h.
//
// The store is one INI-shaped UTF-8 file, parsed by about eighty lines of
// string handling below. GetPrivateProfileString would have saved those lines
// and cost more than they are worth: it is an ANSI API under the covers, so a
// calendar name or a keyword containing a character outside the active code
// page comes back mangled, and it has no notion of a list, so any delimited
// value is one stray semicolon away from silently losing its tail.
//
// Lists are therefore stored as numbered keys -- profile1.name, profile1.link,
// rule7.keyword -- with an explicit count. A value containing the delimiter
// then cannot corrupt the file, because there is no delimiter, and an empty
// list round-trips as count=0 rather than being indistinguishable from a list
// that was never written.
//
// Nothing here throws and nothing here reports a parse failure. A settings file
// is hand-editable by design, and the only sane response to a line that makes
// no sense is to use the default and carry on; degrading to defaults is a
// recoverable annoyance, refusing to start is not.

#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <map>

namespace rc {

namespace {

// The keyword rules live in this file too, but Settings has no member to hold
// them -- they are reached through SaveKeywordRules/LoadKeywordRules rather
// than as a field. Save() rebuilds the whole file in one pass, so it needs the
// current rules to hand or it would erase them; hence this cache, which Load()
// fills and Save() writes back out.
std::vector<KeywordRule> g_keywordRules;

constexpr double kMaxDebugOffset = 3155760000.0;  // ~100 years, per the spec

// An untrusted count still sizes a loop, so it is bounded. Nobody has nine
// hundred calendar profiles; a file claiming they do is corrupt, not ambitious.
constexpr int kMaxListItems = 512;

int CountWords(const std::wstring& normalized) {
    int words = 0;
    bool inWord = false;
    for (wchar_t c : normalized) {
        if (c == L' ') {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++words;
        }
    }
    return words;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// A flat "section.key" -> value map. Nesting would buy nothing: no key is
// looked up without knowing which section it belongs to.
class IniData {
public:
    void Parse(const std::wstring& text) {
        std::wstring section;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t lineEnd = text.find(L'\n', pos);
            if (lineEnd == std::wstring::npos) lineEnd = text.size();

            const std::wstring line = Trim(text.substr(pos, lineEnd - pos));
            pos = lineEnd + 1;
            if (line.empty()) continue;
            if (line[0] == L';' || line[0] == L'#') continue;

            if (line[0] == L'[') {
                const size_t close = line.find(L']');
                if (close != std::wstring::npos) section = Lower(Trim(line.substr(1, close - 1)));
                continue;
            }

            const size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) continue;

            // Only the key is folded and trimmed. The value is taken as written
            // apart from surrounding whitespace, because a calendar name may
            // legitimately contain '=' and any casing it likes.
            const std::wstring key = Lower(Trim(line.substr(0, equals)));
            if (key.empty()) continue;
            values_[section + L"." + key] = Trim(line.substr(equals + 1));
        }
    }

    bool Find(const std::wstring& section, const std::wstring& key, std::wstring* out) const {
        const auto hit = values_.find(section + L"." + Lower(key));
        if (hit == values_.end()) return false;
        *out = hit->second;
        return true;
    }

    // "0 or missing -> default": a stored number has to be greater than zero to
    // displace a default. That rule is what makes a truncated or half-written
    // file harmless, and it is why blockCornerRadius -- whose zero is a real
    // choice, meaning "full capsule" -- has to be read separately.
    double Positive(const std::wstring& section, const std::wstring& key, double fallback) const {
        std::wstring raw;
        if (!Find(section, key, &raw) || raw.empty()) return fallback;
        wchar_t* stop = nullptr;
        const double value = wcstod(raw.c_str(), &stop);
        if (stop == raw.c_str() || !std::isfinite(value) || value <= 0.0) return fallback;
        return value;
    }

    // Signed, zero permitted, for the values whose zero means something.
    double Number(const std::wstring& section, const std::wstring& key, double fallback) const {
        std::wstring raw;
        if (!Find(section, key, &raw) || raw.empty()) return fallback;
        wchar_t* stop = nullptr;
        const double value = wcstod(raw.c_str(), &stop);
        if (stop == raw.c_str() || !std::isfinite(value)) return fallback;
        return value;
    }

    int Int(const std::wstring& section, const std::wstring& key, int fallback) const {
        const double value = Number(section, key, static_cast<double>(fallback));
        if (value > 2147483000.0 || value < -2147483000.0) return fallback;
        return static_cast<int>(value);
    }

    // Booleans cannot use the "0 or missing" rule: a default-true toggle that
    // the user has switched off stores a zero, and treating that zero as absent
    // would switch it straight back on at the next launch.
    bool Bool(const std::wstring& section, const std::wstring& key, bool fallback) const {
        std::wstring raw;
        if (!Find(section, key, &raw)) return fallback;
        const std::wstring value = Lower(raw);
        if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") return true;
        if (value == L"0" || value == L"false" || value == L"no" || value == L"off") return false;
        return fallback;
    }

    std::wstring Text(const std::wstring& section,
                      const std::wstring& key,
                      const std::wstring& fallback) const {
        std::wstring raw;
        if (!Find(section, key, &raw) || raw.empty()) return fallback;
        return raw;
    }

    int Count(const std::wstring& section) const {
        const int n = Int(section, L"count", 0);
        if (n < 0) return 0;
        return (n > kMaxListItems) ? kMaxListItems : n;
    }

    bool Has(const std::wstring& section, const std::wstring& key) const {
        std::wstring ignored;
        return Find(section, key, &ignored);
    }

private:
    std::map<std::wstring, std::wstring> values_;
};

void AppendLine(std::wstring* out, const std::wstring& line) {
    *out += line;
    *out += L"\r\n";
}

void AppendBool(std::wstring* out, const wchar_t* key, bool value) {
    AppendLine(out, Format(L"%s=%d", key, value ? 1 : 0));
}

void AppendNumber(std::wstring* out, const wchar_t* key, double value) {
    // %g rather than %f: these are round numbers in practice and a width of
    // 250.000000 in a file people are expected to edit by hand is just noise.
    AppendLine(out, Format(L"%s=%.10g", key, value));
}

void AppendInt(std::wstring* out, const wchar_t* key, int value) {
    AppendLine(out, Format(L"%s=%d", key, value));
}

void AppendText(std::wstring* out, const wchar_t* key, const std::wstring& value) {
    AppendLine(out, Format(L"%s=%s", key, value.c_str()));
}

}  // namespace

Settings& Settings::Get() {
    // Function-local static: initialised on first use, so nothing here runs
    // before WinMain and no other translation unit's static can depend on the
    // order in which this one was constructed.
    static Settings instance;
    return instance;
}

std::wstring Settings::FilePath() const {
    const std::wstring dir = AppDataDir();
    if (dir.empty()) return std::wstring();
    return dir + L"\\settings.ini";
}

// ------------------------------------------------------------------- loading

void Settings::Load() {
    std::wstring text;
    const std::wstring path = FilePath();
    if (path.empty() || !ReadFileText(path, &text)) {
        // No file is the ordinary first-launch state, not an error. Demo mode
        // is on because there is nothing else to show.
        demoMode = !hasCalendarInput();
        Clock::SetOffset(debugOffset);
        return;
    }

    IniData ini;
    ini.Parse(text);

    // ---- strip appearance ----------------------------------------------
    windowMinutes = ini.Positive(L"strip", L"windowMinutes", 120);
    maxLabelWidth = ini.Positive(L"strip", L"maxLabelWidth", 360);
    showNowName = ini.Bool(L"strip", L"showNowName", true);
    showNowTimeLeft = ini.Bool(L"strip", L"showNowTimeLeft", true);
    showNextName = ini.Bool(L"strip", L"showNextName", true);
    showNextDuration = ini.Bool(L"strip", L"showNextDuration", true);

    // Clamped on read rather than on write. A width of four pixels is not a
    // strip and a width of nine thousand is not a taskbar, and either could
    // arrive from a hand edit or from a settings file copied off a machine with
    // a very different display.
    timelineWidth = ini.Positive(L"strip", L"timelineWidth", 250);
    if (timelineWidth < 50) timelineWidth = 50;
    if (timelineWidth > 900) timelineWidth = 900;

    // ---- hidden settings -------------------------------------------------
    nowLineWidth = ini.Positive(L"hidden", L"nowLineWidth", 4);
    urgentSeconds = ini.Positive(L"hidden", L"urgentSeconds", 120);
    solidBlocks = ini.Bool(L"hidden", L"solidBlocks", true);
    blockGap = ini.Positive(L"hidden", L"blockGap", 1);
    titleFontSize = ini.Positive(L"hidden", L"titleFontSize", 0);
    dayAnchorKeyword = ini.Text(L"hidden", L"dayAnchorKeyword", L"sleep");
    hostOverride = static_cast<int>(ini.Number(L"hidden", L"hostOverride", 0));
    if (hostOverride < 0 || hostOverride > 3) hostOverride = 0;
    pastFade = ini.Number(L"hidden", L"pastFade", 0);
    if (pastFade < 0.0 || pastFade >= 1.0) pastFade = 0;   // 1.0 would be pure white
    blockHeight = ini.Number(L"hidden", L"blockHeight", 0);
    if (blockHeight < 0.0 || blockHeight > 200.0) blockHeight = 0;
    innerGap = ini.Number(L"hidden", L"innerGap", 0);
    if (innerGap < 0.0 || innerGap > 64.0) innerGap = 0;

    // Zero is a legitimate stored value here -- it selects a full capsule -- so
    // this one clamps rather than rejecting.
    blockCornerRadius = ini.Number(L"hidden", L"blockCornerRadius", 0);
    if (!(blockCornerRadius > 0)) blockCornerRadius = 0;

    COLORREF parsed = 0;
    if (ParseHexColor(ini.Text(L"hidden", L"unmatchedColor", L""), &parsed)) unmatchedColor = parsed;

    // ---- calendar source --------------------------------------------------
    calendarUrl = ini.Text(L"calendar", L"calendarUrl", L"");
    if (calendarUrl.empty()) {
        // The pre-profiles key. Read-only: it is adopted as a profile on launch
        // and never written back, so it disappears the first time anything else
        // is saved.
        calendarUrl = ini.Text(L"calendar", L"icsUrl", L"");
    }
    activeProfile = ini.Text(L"calendar", L"activeProfile", L"");

    profiles.clear();
    const int profileCount = ini.Count(L"calendar");
    for (int i = 1; i <= profileCount; ++i) {
        CalendarProfile profile;
        profile.name = ini.Text(L"calendar", Format(L"profile%d.name", i), L"");
        profile.link = ini.Text(L"calendar", Format(L"profile%d.link", i), L"");
        if (profile.name.empty() || profile.link.empty()) continue;
        profiles.push_back(profile);
    }

    // Demo mode is the only setting whose default depends on another, so it is
    // read after the source: with nothing configured there is nothing to show
    // but the demo.
    demoMode = ini.Bool(L"calendar", L"demoMode", !hasCalendarInput());

    // ---- keyword rules ----------------------------------------------------
    keywordRulesSource = ini.Text(L"keywords", L"source", L"the built-in sample");
    keywordRulesSeeded = ini.Bool(L"keywords", L"seeded", false);

    g_keywordRules.clear();
    const int ruleCount = ini.Count(L"keywords");
    for (int i = 1; i <= ruleCount; ++i) {
        KeywordRule rule;
        rule.keyword = ini.Text(L"keywords", Format(L"rule%d.keyword", i), L"");
        if (rule.keyword.empty()) continue;
        rule.category = ini.Text(L"keywords", Format(L"rule%d.category", i),
                                 std::wstring(keywords::kUncategorized));
        rule.colorName = ini.Text(L"keywords", Format(L"rule%d.color", i), L"");
        if (!ParseHexColor(rule.colorName, &rule.color)) rule.color = keywords::kUnmatchedColor;
        rule.normalized = Normalize(rule.keyword);
        if (rule.normalized.empty()) continue;
        rule.wordCount = CountWords(rule.normalized);
        g_keywordRules.push_back(rule);
    }

    // ---- debug time --------------------------------------------------------
    debugOffset = ini.Number(L"debug", L"offset", 0);
    if (!std::isfinite(debugOffset)) debugOffset = 0;
    if (debugOffset > kMaxDebugOffset) debugOffset = kMaxDebugOffset;
    if (debugOffset < -kMaxDebugOffset) debugOffset = -kMaxDebugOffset;
    Clock::SetOffset(debugOffset);

    // ---- widget placement ---------------------------------------------------
    widgetOffsetFromRight = ini.Int(L"widget", L"offsetFromRight", -1);
    if (widgetOffsetFromRight < -1) widgetOffsetFromRight = -1;
    monitorDevice = ini.Text(L"widget", L"monitorDevice", L"");

    // ---- sound hours --------------------------------------------------------
    soundHoursTouched = ini.Bool(L"soundhours", L"touched", false);
    if (ini.Has(L"soundhours", L"count")) {
        std::vector<SoundWindow> windows;
        const int windowCount = ini.Count(L"soundhours");
        for (int i = 1; i <= windowCount; ++i) {
            SoundWindow w;
            w.startMinutes = ini.Int(L"soundhours", Format(L"window%d.start", i), -1);
            w.endMinutes = ini.Int(L"soundhours", Format(L"window%d.end", i), -1);
            if (w.startMinutes < 0 || w.startMinutes > 1439) continue;
            if (w.endMinutes < 0 || w.endMinutes > 1439) continue;
            windows.push_back(w);
        }
        soundHours = windows;
    }
    // Emptying the list is what Off means, so the flag follows the list rather
    // than being trusted on its own.
    soundHoursOn = ini.Bool(L"soundhours", L"on", true) && !soundHours.empty();

    // ---- time block alerts ---------------------------------------------------
    alertLeads.clear();
    const int leadCount = ini.Count(L"alerts");
    for (int i = 1; i <= leadCount; ++i) {
        const int lead = ini.Int(L"alerts", Format(L"lead%d", i), -1);
        if (lead < 0) continue;  // zero is a real choice: alert as it starts
        alertLeads.push_back(lead);
    }
    if (alertLeads.empty()) {
        // The legacy single-lead key, migrated if it says anything.
        const int legacy = ini.Int(L"alerts", L"lead", 0);
        if (legacy > 0) alertLeads.push_back(legacy);
    }
    std::sort(alertLeads.begin(), alertLeads.end(), std::greater<int>());
    alertLeads.erase(std::unique(alertLeads.begin(), alertLeads.end()), alertLeads.end());

    alertSound = ini.Bool(L"alerts", L"sound", false);
    alertSpeech = ini.Bool(L"alerts", L"speech", false);
    if (alertSound && alertSpeech) alertSpeech = false;  // mutually exclusive
    alertSoundName = ini.Text(L"alerts", L"soundName", L"Notify");
    alertVoice = ini.Text(L"alerts", L"voice", L"");

    alertCategories.clear();
    const int categoryCount = ini.Int(L"alerts", L"categoryCount", 0);
    for (int i = 1; i <= categoryCount && i <= kMaxListItems; ++i) {
        const std::wstring name = ini.Text(L"alerts", Format(L"category%d", i), L"");
        if (!name.empty()) alertCategories.push_back(name);
    }

    // ---- westminster chime ----------------------------------------------------
    const std::wstring mode = Lower(ini.Text(L"chime", L"mode", L"off"));
    if (mode == L"hourly") {
        chimeMode = ChimeMode::Hourly;
    } else if (mode == L"quarterly") {
        chimeMode = ChimeMode::Quarterly;
    } else {
        chimeMode = ChimeMode::Off;
    }
    chimeStrikesHour = ini.Bool(L"chime", L"strikesHour", true);

    // Volume zero is not allowed: silence is what Off and Sound Hours are for,
    // and a chime that is on but inaudible reads as a bug.
    const double volume = ini.Positive(L"chime", L"volume", 0.5);
    chimeVolume = static_cast<float>((volume > 1.0) ? 1.0 : volume);

    chimeCustomVolumes.clear();
    const int customCount = ini.Count(L"chime");
    for (int i = 1; i <= customCount; ++i) {
        const int value = ini.Int(L"chime", Format(L"custom%d", i), 0);
        if (value >= 1 && value <= 100) chimeCustomVolumes.push_back(value);
    }

    // ---- run at startup -------------------------------------------------------
    runAtStartup = ini.Bool(L"startup", L"runAtStartup", false);
    startupDelay = ini.Int(L"startup", L"delay", 20);
    if (startupDelay < 0 || startupDelay > 3600) startupDelay = 20;
}

// ------------------------------------------------------------------- saving

void Settings::Save() {
    const std::wstring path = FilePath();
    if (path.empty()) return;

    // The whole file is rebuilt and written in one call. A partial write -- of
    // the kind you get from updating a key in place -- is what leaves a file
    // half in the old format and half in the new after a crash mid-save.
    std::wstring out;
    out.reserve(4096);

    AppendLine(&out, Format(L"; %s settings. Edited by hand at your own risk;", kDisplayName));
    AppendLine(&out, L"; anything unreadable falls back to the default.");
    AppendLine(&out, L"");

    AppendLine(&out, L"[strip]");
    AppendNumber(&out, L"windowMinutes", windowMinutes);
    AppendNumber(&out, L"timelineWidth", timelineWidth);
    AppendNumber(&out, L"maxLabelWidth", maxLabelWidth);
    AppendBool(&out, L"showNowName", showNowName);
    AppendBool(&out, L"showNowTimeLeft", showNowTimeLeft);
    AppendBool(&out, L"showNextName", showNextName);
    AppendBool(&out, L"showNextDuration", showNextDuration);
    AppendLine(&out, L"");

    AppendLine(&out, L"[hidden]");
    AppendNumber(&out, L"nowLineWidth", nowLineWidth);
    AppendNumber(&out, L"urgentSeconds", urgentSeconds);
    AppendText(&out, L"unmatchedColor", ColorToHex(unmatchedColor));
    AppendBool(&out, L"solidBlocks", solidBlocks);
    AppendNumber(&out, L"blockGap", blockGap);
    AppendNumber(&out, L"blockCornerRadius", blockCornerRadius);
    AppendNumber(&out, L"titleFontSize", titleFontSize);
    AppendText(&out, L"dayAnchorKeyword", dayAnchorKeyword);
    AppendNumber(&out, L"hostOverride", hostOverride);
    AppendNumber(&out, L"pastFade", pastFade);
    AppendNumber(&out, L"blockHeight", blockHeight);
    AppendNumber(&out, L"innerGap", innerGap);
    AppendLine(&out, L"");

    AppendLine(&out, L"[calendar]");
    AppendText(&out, L"calendarUrl", calendarUrl);
    AppendText(&out, L"activeProfile", activeProfile);
    AppendBool(&out, L"demoMode", demoMode);
    AppendInt(&out, L"count", static_cast<int>(profiles.size()));
    for (size_t i = 0; i < profiles.size(); ++i) {
        const int n = static_cast<int>(i) + 1;
        AppendText(&out, Format(L"profile%d.name", n).c_str(), profiles[i].name);
        AppendText(&out, Format(L"profile%d.link", n).c_str(), profiles[i].link);
    }
    AppendLine(&out, L"");

    AppendLine(&out, L"[keywords]");
    AppendText(&out, L"source", keywordRulesSource);
    AppendBool(&out, L"seeded", keywordRulesSeeded);
    AppendInt(&out, L"count", static_cast<int>(g_keywordRules.size()));
    for (size_t i = 0; i < g_keywordRules.size(); ++i) {
        const int n = static_cast<int>(i) + 1;
        AppendText(&out, Format(L"rule%d.category", n).c_str(), g_keywordRules[i].category);
        AppendText(&out, Format(L"rule%d.color", n).c_str(), ColorToHex(g_keywordRules[i].color));
        AppendText(&out, Format(L"rule%d.keyword", n).c_str(), g_keywordRules[i].keyword);
    }
    AppendLine(&out, L"");

    AppendLine(&out, L"[debug]");
    AppendNumber(&out, L"offset", debugOffset);
    AppendLine(&out, L"");

    AppendLine(&out, L"[widget]");
    AppendInt(&out, L"offsetFromRight", widgetOffsetFromRight);
    AppendText(&out, L"monitorDevice", monitorDevice);
    AppendLine(&out, L"");

    AppendLine(&out, L"[soundhours]");
    AppendBool(&out, L"on", soundHoursOn);
    AppendBool(&out, L"touched", soundHoursTouched);
    AppendInt(&out, L"count", static_cast<int>(soundHours.size()));
    for (size_t i = 0; i < soundHours.size(); ++i) {
        const int n = static_cast<int>(i) + 1;
        AppendInt(&out, Format(L"window%d.start", n).c_str(), soundHours[i].startMinutes);
        AppendInt(&out, Format(L"window%d.end", n).c_str(), soundHours[i].endMinutes);
    }
    AppendLine(&out, L"");

    AppendLine(&out, L"[alerts]");
    AppendInt(&out, L"count", static_cast<int>(alertLeads.size()));
    for (size_t i = 0; i < alertLeads.size(); ++i) {
        AppendInt(&out, Format(L"lead%d", static_cast<int>(i) + 1).c_str(), alertLeads[i]);
    }
    AppendBool(&out, L"sound", alertSound);
    AppendBool(&out, L"speech", alertSpeech);
    AppendText(&out, L"soundName", alertSoundName);
    AppendText(&out, L"voice", alertVoice);
    AppendInt(&out, L"categoryCount", static_cast<int>(alertCategories.size()));
    for (size_t i = 0; i < alertCategories.size(); ++i) {
        AppendText(&out, Format(L"category%d", static_cast<int>(i) + 1).c_str(),
                   alertCategories[i]);
    }
    AppendLine(&out, L"");

    AppendLine(&out, L"[chime]");
    const wchar_t* modeText = L"off";
    if (chimeMode == ChimeMode::Hourly) modeText = L"hourly";
    if (chimeMode == ChimeMode::Quarterly) modeText = L"quarterly";
    AppendText(&out, L"mode", modeText);
    AppendBool(&out, L"strikesHour", chimeStrikesHour);
    AppendNumber(&out, L"volume", static_cast<double>(chimeVolume));
    AppendInt(&out, L"count", static_cast<int>(chimeCustomVolumes.size()));
    for (size_t i = 0; i < chimeCustomVolumes.size(); ++i) {
        AppendInt(&out, Format(L"custom%d", static_cast<int>(i) + 1).c_str(),
                  chimeCustomVolumes[i]);
    }
    AppendLine(&out, L"");

    AppendLine(&out, L"[startup]");
    AppendBool(&out, L"runAtStartup", runAtStartup);
    AppendInt(&out, L"delay", startupDelay);

    WriteFileText(path, out);
}

// ------------------------------------------------------------------ restoring

void Settings::Reseed() {
    // Clearing the flag is what lets the sample rule set be seeded again. It is
    // separate from the rules themselves because the flag is also what makes
    // "cleared stays cleared" work in the ordinary case.
    keywordRulesSeeded = false;
    keywordRulesSource = L"the built-in sample";
    g_keywordRules.clear();
}

void Settings::RestoreAll() {
    const std::wstring path = FilePath();
    if (!path.empty()) ::DeleteFileW(path.c_str());

    // Copying from a freshly constructed instance rather than assigning each
    // field by hand: a setting added later then cannot be missed by the reset,
    // which is the same reason the file is deleted whole rather than key by key.
    Settings fresh;
    *this = fresh;

    Reseed();
    Clock::SetOffset(0);
    Save();
}

void Settings::RestoreStrip() {
    windowMinutes = 120;
    timelineWidth = 250;
    maxLabelWidth = 360;
    showNowName = true;
    showNowTimeLeft = true;
    showNextName = true;
    showNextDuration = true;
    Save();
}

bool Settings::isAppearanceDefault() const {
    return windowMinutes == 120 && timelineWidth == 250 && maxLabelWidth == 360 && showNowName &&
           showNowTimeLeft && showNextName && showNextDuration;
}

bool Settings::isEverythingDefault() const {
    // Asked feature by feature rather than by checking whether the file exists.
    // A first launch writes keys of its own -- the seeded flag, the demo-mode
    // decision, the widget's resting position -- so "a settings file is
    // present" is true within seconds of installing and would grey out Restore
    // Defaults for a user who has changed nothing and one who has changed
    // everything alike.
    if (!isAppearanceDefault()) return false;

    if (!profiles.empty() || !demoMode || !calendarUrl.empty() || !activeProfile.empty()) {
        return false;
    }

    // The keyword rules count as default when they are still the built-in
    // sample: an import or a clear both change the source name.
    if (keywordRulesSource != L"the built-in sample") return false;
    if (keywords::SourceName() != L"the built-in sample") return false;
    if (keywords::Rules().size() != keywords::SampleRules().size()) return false;

    if (!alertLeads.empty() || alertSound || alertSpeech) return false;

    if (chimeMode != ChimeMode::Off || !chimeStrikesHour) return false;
    if (std::fabs(static_cast<double>(chimeVolume) - 0.5) > 0.001) return false;
    if (!chimeCustomVolumes.empty()) return false;

    if (!soundHoursOn || soundHoursTouched) return false;
    if (soundHours.size() != 1) return false;
    if (!(soundHours[0] == SoundWindow{690, 270})) return false;

    if (runAtStartup || startupDelay != 20) return false;

    if (debugOffset != 0) return false;

    return true;
}

// ------------------------------------------------------------------- profiles

void Settings::AddProfile(const std::wstring& name, const std::wstring& link) {
    if (name.empty() || link.empty()) return;

    // Adding a name that already exists replaces it. The alternative is two
    // rows reading "Work" with different links behind them, which no amount of
    // menu design makes usable.
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(),
                                  [&name](const CalendarProfile& p) {
                                      return EqualsNoCase(p.name, name);
                                  }),
                   profiles.end());

    CalendarProfile added;
    added.name = name;
    added.link = link;
    profiles.push_back(added);

    // Case-insensitive, because "work" and "Work" sitting apart in the list
    // looks like a bug even when it is technically a sort.
    std::sort(profiles.begin(), profiles.end(),
              [](const CalendarProfile& a, const CalendarProfile& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    ActivateProfile(name);  // saves
}

void Settings::RenameProfile(const std::wstring& oldName, const std::wstring& newName) {
    if (newName.empty()) return;

    bool renamed = false;
    for (CalendarProfile& p : profiles) {
        if (EqualsNoCase(p.name, oldName)) {
            p.name = newName;
            renamed = true;
            break;
        }
    }
    if (!renamed) return;

    if (EqualsNoCase(activeProfile, oldName)) activeProfile = newName;

    std::sort(profiles.begin(), profiles.end(),
              [](const CalendarProfile& a, const CalendarProfile& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    Save();
}

void Settings::RemoveProfile(const std::wstring& name) {
    const size_t before = profiles.size();
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(),
                                  [&name](const CalendarProfile& p) {
                                      return EqualsNoCase(p.name, name);
                                  }),
                   profiles.end());
    if (profiles.size() == before) return;

    if (!EqualsNoCase(activeProfile, name)) {
        Save();
        return;
    }

    if (!profiles.empty()) {
        ActivateProfile(profiles[0].name);  // saves
        return;
    }

    // Nothing left to read. Only the saved link is forgotten here: the app has
    // never written to the calendar itself and there is nothing on the far end
    // of that URL to undo.
    activeProfile.clear();
    calendarUrl.clear();
    Save();
}

void Settings::ActivateProfile(const std::wstring& name) {
    for (const CalendarProfile& p : profiles) {
        if (!EqualsNoCase(p.name, name)) continue;
        calendarUrl = p.link;
        activeProfile = p.name;
        demoMode = false;
        Save();
        return;
    }
}

std::wstring Settings::SourceDisplayName() const {
    if (demoMode) return L"Demo Calendar (test data)";
    if (!activeProfile.empty()) return activeProfile;
    if (!calendarUrl.empty()) return calendarUrl;
    return L"No calendar yet";
}

// --------------------------------------------------------------- keyword rules

void Settings::SaveKeywordRules(const std::vector<KeywordRule>& rules) {
    g_keywordRules = rules;
    Save();
}

std::vector<KeywordRule> Settings::LoadKeywordRules() const {
    // An empty vector when none are stored, which the caller reads as "seed the
    // sample" on a first launch and as "the user cleared them" afterwards. The
    // seeded flag is what tells those two apart.
    return g_keywordRules;
}

}  // namespace rc
