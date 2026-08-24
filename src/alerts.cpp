// alerts.cpp — the firing engine, the sound list, the voices, the categories.
//
// See alerts.h for the contract. The engine is a single pass over every loaded
// event once a second, which sounds wasteful and is not: a day's calendar is a
// few dozen events, and the alternative -- a sorted queue of pending alerts --
// has to be invalidated and rebuilt on every refetch, every lead-set change and
// every category change, which is three more chances to lose an alert.
//
// Speech is SAPI 5 rather than the WinRT synthesiser: SAPI is present on every
// supported Windows, needs no manifest and no async plumbing, and ISpVoice
// already speaks off-thread for us when asked with SPF_ASYNC.

#include "alerts.h"

// objbase.h ahead of sapi.h: a build that defines WIN32_LEAN_AND_MEAN keeps
// windows.h from pulling COM in, and sapi.h assumes it is already there.
#include <objbase.h>

#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <cwchar>
#include <functional>
#include <unordered_map>
#include <utility>

#include "dialogs.h"
#include "keywords.h"
#include "raii.h"
#include "settings.h"
#include "soundhours.h"

namespace rc {
namespace alerts {
namespace {

// ---------------------------------------------------------------- module state
//
// All of this is touched from the UI thread only -- Tick, the menu handlers and
// the wake notification all arrive there -- so none of it is locked.

ComPtr<ISpVoice> g_voice;
std::wstring g_voiceIdInUse;   // avoids re-resolving the token on every alert

std::vector<Voice> g_voices;
bool g_voicesLoaded = false;

struct SoundEntry {
    std::wstring name;   // bare, no extension: what the menu shows
    std::wstring path;
};
std::vector<SoundEntry> g_sounds;
bool g_soundsLoaded = false;

// Memory-only on purpose. Persisting it would mean a relaunch could inherit
// "already fired" for an alert it never actually made, and the cost of the
// alternative is at worst one duplicate announcement after a restart.
std::unordered_map<std::wstring, bool> g_fired;
Seconds g_lastPrune = 0;

constexpr size_t kPruneThreshold = 200;
constexpr size_t kHardClearThreshold = 5000;
constexpr Seconds kPruneInterval = 60;
constexpr Seconds kPruneCutoff = 3600;

// ---------------------------------------------------------------- small helpers

// Frees a string SAPI handed back through CoTaskMemAlloc.
class CoStr {
public:
    CoStr() = default;
    ~CoStr() { ::CoTaskMemFree(p_); }
    CoStr(const CoStr&) = delete;
    CoStr& operator=(const CoStr&) = delete;

    LPWSTR* put() {
        ::CoTaskMemFree(p_);
        p_ = nullptr;
        return &p_;
    }
    std::wstring str() const { return p_ ? std::wstring(p_) : std::wstring(); }
    bool valid() const { return p_ != nullptr && *p_ != 0; }

private:
    LPWSTR p_ = nullptr;
};

bool ContainsCI(const std::wstring& haystack, const std::wstring& needle) {
    return Lower(haystack).find(needle) != std::wstring::npos;
}

// ---------------------------------------------------------------- sound list

void ScanWavDirectory(const std::wstring& dir, std::vector<SoundEntry>* out) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\*.wav";
    const HANDLE find = ::FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring name(fd.cFileName);
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.erase(dot);
        if (name.empty()) continue;

        // A user import of the same name shadows nothing: first directory
        // scanned wins, and %WINDIR%\Media is scanned first.
        const bool already = std::any_of(out->begin(), out->end(), [&](const SoundEntry& e) {
            return Lower(e.name) == Lower(name);
        });
        if (already) continue;

        out->push_back({name, dir + L"\\" + fd.cFileName});
    } while (::FindNextFileW(find, &fd));

    ::FindClose(find);
}

// The quieter notification sounds, in the order they should appear. A default
// alert list that opens with a fanfare teaches people to turn alerts off.
const wchar_t* const kPreferredSounds[] = {
    L"windows notify", L"windows ding",  L"windows background",
    L"windows message nudge", L"chimes", L"chord", L"ding", L"notify", L"tada",
};

void LoadSounds() {
    g_sounds.clear();

    wchar_t windir[MAX_PATH] = {};
    if (::GetWindowsDirectoryW(windir, MAX_PATH) > 0) {
        ScanWavDirectory(std::wstring(windir) + L"\\Media", &g_sounds);
    }
    ScanWavDirectory(AppDataDir() + L"\\Sounds", &g_sounds);

    std::vector<SoundEntry> ordered;
    ordered.reserve(g_sounds.size());
    std::vector<bool> taken(g_sounds.size(), false);

    for (const wchar_t* pattern : kPreferredSounds) {
        for (size_t i = 0; i < g_sounds.size(); ++i) {
            if (taken[i]) continue;
            if (!ContainsCI(g_sounds[i].name, pattern)) continue;
            taken[i] = true;
            ordered.push_back(g_sounds[i]);
        }
    }

    std::vector<SoundEntry> rest;
    for (size_t i = 0; i < g_sounds.size(); ++i) {
        if (!taken[i]) rest.push_back(g_sounds[i]);
    }
    std::sort(rest.begin(), rest.end(), [](const SoundEntry& a, const SoundEntry& b) {
        return Lower(a.name) < Lower(b.name);
    });

    ordered.insert(ordered.end(), rest.begin(), rest.end());
    g_sounds = std::move(ordered);
    g_soundsLoaded = true;
}

std::wstring PathForSound(const std::wstring& name) {
    if (!g_soundsLoaded) LoadSounds();
    const std::wstring wanted = Lower(name);
    for (const SoundEntry& e : g_sounds) {
        if (Lower(e.name) == wanted) return e.path;
    }
    return std::wstring();
}

void PlayNamedSound(const std::wstring& name) {
    const std::wstring path = PathForSound(name);
    if (path.empty()) return;
    // SND_NODEFAULT: if the file has gone the app stays silent rather than
    // playing the system default, which would be an alert the user never chose.
    ::PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

// ---------------------------------------------------------------- voice list

// The Language attribute is a hex LCID. These adjectives read better in a menu
// than "English (United Kingdom)" does, and the table only has to cover the
// languages voices actually ship in; anything else falls back to the OS name.
struct LangName {
    unsigned lcid;
    const wchar_t* label;
};
const LangName kLangNames[] = {
    {0x0409, L"American"},  {0x0809, L"British"},   {0x0c09, L"Australian"},
    {0x1009, L"Canadian"},  {0x1409, L"NewZealand"},{0x1809, L"Irish"},
    {0x1c09, L"SouthAfrican"}, {0x2009, L"Indian"}, {0x040c, L"French"},
    {0x0407, L"German"},    {0x0410, L"Italian"},   {0x0c0a, L"Spanish"},
    {0x0416, L"Brazilian"}, {0x0816, L"Portuguese"},{0x0419, L"Russian"},
    {0x0413, L"Dutch"},     {0x041d, L"Swedish"},   {0x0415, L"Polish"},
    {0x0411, L"Japanese"},  {0x0412, L"Korean"},    {0x0804, L"Chinese"},
    {0x0414, L"Norwegian"}, {0x0406, L"Danish"},    {0x040b, L"Finnish"},
    {0x041f, L"Turkish"},   {0x0405, L"Czech"},     {0x040e, L"Hungarian"},
    {0x0408, L"Greek"},     {0x0401, L"Arabic"},    {0x040d, L"Hebrew"},
    {0x0439, L"Hindi"},
};

std::wstring LanguageLabel(const std::wstring& attribute) {
    if (attribute.empty()) return std::wstring();

    // The attribute can be a semicolon-separated list; the first entry is the
    // voice's primary language and the rest are what it can also read.
    std::wstring first = attribute;
    const size_t semi = first.find(L';');
    if (semi != std::wstring::npos) first.erase(semi);
    first = Trim(first);
    if (first.empty()) return std::wstring();

    const unsigned lcid = static_cast<unsigned>(::wcstoul(first.c_str(), nullptr, 16));
    for (const LangName& l : kLangNames) {
        if (l.lcid == lcid) return l.label;
    }

    wchar_t buffer[128] = {};
    if (::GetLocaleInfoW(static_cast<LCID>(lcid), LOCALE_SENGLISHLANGUAGENAME, buffer, 128) > 0) {
        return buffer;
    }
    return std::wstring();
}

// "Microsoft George Desktop - English (United Kingdom)" -> "George". The full
// registry name is accurate and unreadable; the given name is what people call
// the voice when they talk about it.
std::wstring ShortVoiceName(const std::wstring& raw) {
    std::wstring s = Trim(raw);
    const size_t dash = s.find(L" - ");
    if (dash != std::wstring::npos) s.erase(dash);
    if (StartsWith(Lower(s), L"microsoft ")) s.erase(0, 10);

    static const wchar_t* const kSuffixes[] = {L" desktop", L" mobile"};
    for (const wchar_t* suffix : kSuffixes) {
        const std::wstring lower = Lower(s);
        const std::wstring suf(suffix);
        if (lower.size() >= suf.size() &&
            lower.compare(lower.size() - suf.size(), suf.size(), suf) == 0) {
            s.erase(s.size() - suf.size());
        }
    }
    return Trim(s);
}

std::wstring AttributeValue(ISpObjectToken* token, const wchar_t* name) {
    ComPtr<ISpDataKey> key;
    if (FAILED(token->OpenKey(L"Attributes", key.put()))) return std::wstring();
    CoStr value;
    if (FAILED(key->GetStringValue(name, value.put()))) return std::wstring();
    return value.str();
}

void LoadVoices() {
    g_voices.clear();
    g_voicesLoaded = true;

    ComPtr<ISpObjectTokenCategory> category;
    if (FAILED(::CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                  IID_ISpObjectTokenCategory,
                                  reinterpret_cast<void**>(category.put())))) {
        return;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) return;

    ComPtr<IEnumSpObjectTokens> tokens;
    if (FAILED(category->EnumTokens(nullptr, nullptr, tokens.put()))) return;

    for (;;) {
        ComPtr<ISpObjectToken> token;
        ULONG fetched = 0;
        if (tokens->Next(1, token.put(), &fetched) != S_OK || fetched == 0) break;

        CoStr id;
        if (FAILED(token->GetId(id.put())) || !id.valid()) continue;

        const std::wstring language = LanguageLabel(AttributeValue(token.get(), L"Language"));
        const std::wstring gender = Lower(AttributeValue(token.get(), L"Gender"));
        const std::wstring name = ShortVoiceName(AttributeValue(token.get(), L"Name"));

        std::wstring label;
        for (const std::wstring& part : {language, gender, name}) {
            if (part.empty()) continue;
            if (!label.empty()) label += L" - ";
            label += part;
        }
        if (label.empty()) {
            // Nothing derivable: the token's own description is the last
            // resort, and is never empty for a voice Windows will actually use.
            CoStr description;
            if (SUCCEEDED(token->GetStringValue(nullptr, description.put()))) {
                label = description.str();
            }
        }
        if (label.empty()) label = id.str();

        g_voices.push_back({id.str(), label});
    }
}

bool SelectVoice(const std::wstring& id) {
    if (!g_voice) return false;
    if (id.empty()) return true;          // empty means "whatever SAPI defaults to"
    if (id == g_voiceIdInUse) return true;

    ComPtr<ISpObjectToken> token;
    if (FAILED(::CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken,
                                  reinterpret_cast<void**>(token.put())))) {
        return false;
    }
    if (FAILED(token->SetId(nullptr, id.c_str(), FALSE))) return false;
    if (FAILED(g_voice->SetVoice(token.get()))) return false;

    g_voiceIdInUse = id;
    return true;
}

// ---------------------------------------------------------------- dedupe map

void PruneFiredMap(Seconds now) {
    // Runaway guard first. Above five thousand entries something has gone wrong
    // -- a feed generating events in a loop, a clock jumping -- and the cheapest
    // correct answer is to forget everything and risk one repeat.
    if (g_fired.size() > kHardClearThreshold) {
        g_fired.clear();
        g_lastPrune = RealNow();
        return;
    }
    if (g_fired.size() <= kPruneThreshold) return;

    // Real time, not simulated: this is housekeeping, and a debug offset of two
    // weeks must not make it run every tick.
    const Seconds realNow = RealNow();
    if (realNow - g_lastPrune < kPruneInterval) return;
    g_lastPrune = realNow;

    const Seconds cutoff = now - kPruneCutoff;
    for (auto it = g_fired.begin(); it != g_fired.end();) {
        // The key opens with the start epoch, which is all the age test needs.
        const Seconds start = ::wcstoll(it->first.c_str(), nullptr, 10);
        if (start < cutoff) {
            it = g_fired.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- announcing

void Announce(const std::vector<std::wstring>& titles, int lead) {
    const Settings& cfg = Cfg();
    if (cfg.alertSpeech) {
        Speak(SpeechText(titles, lead));
    } else if (cfg.alertSound) {
        PlayNamedSound(cfg.alertSoundName);
    }
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

void Init() {
    // COM is already initialised on the UI thread by the app; SAPI needs
    // nothing else. A failure here is not fatal -- the app simply cannot speak,
    // and the sound path is untouched.
    ::CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                       reinterpret_cast<void**>(g_voice.put()));
    g_voiceIdInUse.clear();

    RefreshVoices();
    RefreshSounds();
}

void Shutdown() {
    if (g_voice) {
        // Anything still queued is about to be pointless, and SAPI will
        // otherwise hold the process open until it has finished the sentence.
        g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    }
    g_voice.reset();
    g_voiceIdInUse.clear();
    g_voices.clear();
    g_voicesLoaded = false;
    g_sounds.clear();
    g_soundsLoaded = false;
    g_fired.clear();
}

// ---------------------------------------------------------------- the engine

void Tick(const std::vector<CalEvent>& allEvents, Seconds now, const TimeZone& zone) {
    const Settings& cfg = Cfg();
    if (!cfg.alertsEnabled()) return;

    PruneFiredMap(now);

    const int longestLead = cfg.alertLeads.front();   // sorted descending

    // Lead -> the names to announce for it. Several leads can come due in the
    // same second (a one-minute alert and a zero-second alert on a block that
    // starts in under a second), and announcing both would talk over itself.
    std::vector<std::pair<int, std::vector<std::wstring>>> pending;

    for (const CalEvent& e : allEvents) {
        // All-day events never alert. "Out of office starts in ten minutes" at
        // ten to midnight is noise, not information.
        if (e.isAllDay) continue;

        const std::wstring category = e.category.empty() ? keywords::kUncategorized : e.category;
        if (!CategoryAllowed(category)) continue;

        const Seconds delta = e.start - now;

        // The lower bound is exclusive at -30 rather than 0 so that a lead of
        // zero can fire *as* the block begins: on the tick where delta is
        // exactly 0 the event is already, strictly, no longer in the future.
        if (delta <= -kLatenessGrace || delta > longestLead) continue;

        for (int lead : cfg.alertLeads) {
            if (delta > lead) continue;

            const std::wstring key =
                Format(L"%lld|%d|%s", static_cast<long long>(e.start), lead, e.title.c_str());
            if (g_fired.find(key) != g_fired.end()) continue;
            g_fired[key] = true;

            // The lateness rule. An alert more than thirty seconds late is not
            // a late alert, it is a wrong one: saying "ten minutes before Focus
            // Work" when there are three minutes left states something untrue.
            // The machine was asleep, or the app has just launched into the
            // middle of the window. Mark it done and say nothing.
            if (lead - delta > kLatenessGrace) continue;

            auto slot = std::find_if(pending.begin(), pending.end(),
                                     [lead](const std::pair<int, std::vector<std::wstring>>& p) {
                                         return p.first == lead;
                                     });
            if (slot == pending.end()) {
                pending.push_back({lead, {e.title}});
            } else {
                slot->second.push_back(e.title);
            }
        }
    }

    if (pending.empty()) return;

    // Only the nearest lead speaks. Its wording is the one that is true for the
    // longest -- "starting now" beats "one minute before" when both are due.
    auto nearest = std::min_element(pending.begin(), pending.end(),
                                    [](const std::pair<int, std::vector<std::wstring>>& a,
                                       const std::pair<int, std::vector<std::wstring>>& b) {
                                        return a.first < b.first;
                                    });

    // The gate is checked here and nowhere earlier on purpose: every event above
    // has already been marked fired. Sound Hours withholds the noise, it does
    // not defer it, so leaving the quiet period must not release a backlog.
    if (!soundhours::Allows(now, zone)) return;

    Announce(nearest->second, nearest->first);
}

void TestNow() {
    // Exempt from Sound Hours: a button labelled "Test Alert Now" that does
    // nothing because it is half past eleven is indistinguishable from a bug.
    const Settings& cfg = Cfg();
    const std::vector<std::wstring> titles{L"Focus Work | Learn"};
    if (cfg.alertSpeech) {
        Speak(SpeechText(titles, 0));
    } else {
        // Falls back to the sound even when neither is switched on, because the
        // point of the button is to hear something.
        PlayNamedSound(cfg.alertSoundName);
    }
}

void ResetFiredMap() {
    g_fired.clear();
    g_lastPrune = RealNow();
}

// ---------------------------------------------------------------- lead times

void SetLeads(std::vector<int> seconds) {
    std::vector<int> clean;
    clean.reserve(seconds.size());
    for (int s : seconds) {
        // Zero is legitimate -- "when it starts" is a real choice. Only a
        // negative lead, which would mean alerting after the fact, is dropped.
        if (s < 0) continue;
        if (std::find(clean.begin(), clean.end(), s) == clean.end()) clean.push_back(s);
    }
    std::sort(clean.begin(), clean.end(), std::greater<int>());

    Settings& cfg = Cfg();
    cfg.alertLeads = std::move(clean);
    ResetFiredMap();
    cfg.Save();
}

std::wstring DescribeLeads() {
    const Settings& cfg = Cfg();
    if (cfg.alertLeads.empty()) return L"Off";

    std::wstring out;
    for (int s : cfg.alertLeads) {
        if (!out.empty()) out += L", ";
        if (s == 0) {
            // "0m" in a menu title reads as a mistake; the preset is called
            // "When it starts", so the summary says the same thing.
            out += L"start";
        } else if (s % 60 == 0) {
            out += Format(L"%dm", s / 60);
        } else {
            out += Format(L"%ds", s);
        }
    }
    return out;
}

std::wstring LeadPhrase(int seconds) {
    if (seconds <= 0) return L"now";   // lead 0 is phrased by SpeechText, not here
    if (seconds % 60 == 0) {
        const int minutes = seconds / 60;
        return Format(L"%d %s", minutes, minutes == 1 ? L"minute" : L"minutes");
    }
    if (seconds < 60) {
        return Format(L"%d %s", seconds, seconds == 1 ? L"second" : L"seconds");
    }
    // 90 seconds is "1.5 minutes", not "1 minute 30 seconds": a synthesiser
    // reads the decimal cleanly and the compound phrase awkwardly.
    return Format(L"%.1f minutes", seconds / 60.0);
}

// ---------------------------------------------------------------- sound

std::vector<std::wstring> AvailableSounds() {
    if (!g_soundsLoaded) LoadSounds();
    std::vector<std::wstring> names;
    names.reserve(g_sounds.size());
    for (const SoundEntry& e : g_sounds) names.push_back(e.name);
    return names;
}

void RefreshSounds() { LoadSounds(); }

void PreviewSound(const std::wstring& name) { PlayNamedSound(name); }

bool ImportSound(HWND owner) {
    std::wstring source;
    if (!dialogs::OpenFile(owner, L"Import Alert Sound", L"Wave audio", L"*.wav", &source)) {
        return false;
    }

    const std::wstring folder = AppDataDir() + L"\\Sounds";
    ::CreateDirectoryW(folder.c_str(), nullptr);

    std::wstring file = source;
    const size_t slash = file.find_last_of(L"\\/");
    if (slash != std::wstring::npos) file.erase(0, slash + 1);
    if (file.empty()) return false;

    // Copied rather than referenced: a sound on a memory stick that is not
    // plugged in would otherwise become a silent alert with no explanation.
    if (!::CopyFileW(source.c_str(), (folder + L"\\" + file).c_str(), FALSE)) return false;

    RefreshSounds();
    return true;
}

// ---------------------------------------------------------------- speech

std::vector<Voice> AvailableVoices() {
    if (!g_voicesLoaded) LoadVoices();
    return g_voices;
}

void RefreshVoices() {
    LoadVoices();
    g_voiceIdInUse.clear();   // a voice may have been uninstalled under us
}

void Speak(const std::wstring& text) {
    if (!g_voice || text.empty()) return;
    if (!SelectVoice(Cfg().alertVoice)) {
        // The stored voice has gone. Speak in whatever SAPI defaults to rather
        // than staying silent, and stop trying to resolve the missing token.
        g_voiceIdInUse.clear();
    }
    // SPF_ASYNC because this is called from the one-second tick on the UI
    // thread, and a spoken sentence takes seconds. SPF_PURGEBEFORESPEAK so a
    // new alert replaces an unfinished one instead of queueing behind it.
    g_voice->Speak(text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
}

std::wstring SpeechText(const std::vector<std::wstring>& titles, int leadSeconds) {
    std::vector<std::wstring> names;
    names.reserve(titles.size());
    for (const std::wstring& title : titles) {
        // The part before the first bar. "Focus Work | Learn" read aloud in
        // full lands as "Focus Work ... Learn", and the pause sounds like a
        // fault rather than punctuation.
        std::wstring name = title;
        const size_t bar = name.find(L'|');
        if (bar != std::wstring::npos) name.erase(bar);
        name = Trim(name);
        if (!name.empty()) names.push_back(name);
    }
    if (names.empty()) return std::wstring();

    std::wstring joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) joined += (i + 1 == names.size()) ? L" and " : L", ";
        joined += names[i];
    }

    if (leadSeconds == 0) return joined + L", starting now";
    return LeadPhrase(leadSeconds) + L" before " + joined;
}

// ---------------------------------------------------------------- categories

namespace {

// Every category a rule set can produce, plus the bucket for titles that match
// no rule at all. This is recomputed rather than cached because importing a CSV
// changes it, and a stale copy here would silently exclude the new categories.
std::vector<std::wstring> AllCategories() {
    std::vector<std::wstring> all;
    for (const keywords::CategorySummary& c : keywords::Categories()) all.push_back(c.name);
    all.push_back(keywords::kUncategorized);
    return all;
}

}  // namespace

void ToggleCategory(const std::wstring& category) {
    Settings& cfg = Cfg();
    const std::vector<std::wstring> all = AllCategories();

    std::vector<std::wstring> selected = cfg.alertCategories;
    if (selected.empty()) {
        // The first click on a category means "everything but this one" -- it
        // is what people reach for when one noisy category is the problem. So
        // seed with the whole set and let the removal below do the work.
        selected = all;
    }

    const auto it = std::find(selected.begin(), selected.end(), category);
    if (it != selected.end()) {
        selected.erase(it);
    } else {
        selected.push_back(category);
    }

    // Empty means "none", which is just alerts switched off by another name,
    // and a full set is "all" written the long way. Both revert to the stored
    // empty vector, so a category added later is included rather than excluded.
    if (selected.empty() || selected.size() >= all.size()) {
        cfg.alertCategories.clear();
    } else {
        cfg.alertCategories = std::move(selected);
    }
    cfg.Save();
}

bool CategoryAllowed(const std::wstring& category) {
    const Settings& cfg = Cfg();
    if (cfg.alertCategories.empty()) return true;
    return std::find(cfg.alertCategories.begin(), cfg.alertCategories.end(), category) !=
           cfg.alertCategories.end();
}

std::wstring DescribeCategories() {
    const Settings& cfg = Cfg();
    if (cfg.alertCategories.empty()) return L"all";
    return Format(L"%d of %d", static_cast<int>(cfg.alertCategories.size()),
                  static_cast<int>(AllCategories().size()));
}

}  // namespace alerts
}  // namespace rc
