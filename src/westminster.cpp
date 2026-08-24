// westminster.cpp — the Westminster Quarters, built out of sine partials.
//
// Nothing is bundled and nothing is downloaded, for a reason that is legal
// rather than technical: a recording of Big Ben is copyright, and the tune,
// written in 1793, is not. So the app synthesises it. That also means the pitch,
// the timing and the volume are parameters rather than properties of a file.
//
// Audio goes out through winmm's waveOut. It is the oldest API on the box and
// exactly the right size for the job: a few seconds of mono 22,050 Hz PCM,
// rendered once and handed over in a single buffer. WASAPI would mean an
// IMMDeviceEnumerator, an activation, a mix-format negotiation and a render
// thread, and XAudio2 a redistributable-era dependency and a COM lifetime, all
// to play one buffer that is already complete before playback starts.

#include "westminster.h"

#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "raii.h"
#include "soundhours.h"

namespace rc {
namespace westminster {
namespace {

// ---------------------------------------------------------------- the tune

constexpr int kSampleRate = 22050;
constexpr double kPi = 3.14159265358979323846;

// Section 12.1, verbatim. E major.
constexpr double kGSharp = 415.30;
constexpr double kFSharp = 369.99;
constexpr double kE4 = 329.63;
constexpr double kB3 = 246.94;
constexpr double kHourBell = 164.81;   // E3

const double kChanges[5][4] = {
    {kGSharp, kFSharp, kE4, kB3},      // 1
    {kE4, kGSharp, kFSharp, kB3},      // 2
    {kE4, kFSharp, kGSharp, kE4},      // 3
    {kGSharp, kE4, kFSharp, kB3},      // 4
    {kB3, kFSharp, kGSharp, kE4},      // 5
};

// Timing, in seconds.
constexpr double kNoteGap = 1.2;
constexpr double kChangeGap = 0.6;    // added after each change, not each note
constexpr double kBeforeStrikes = 2.5;
constexpr double kStrikeGap = 4.0;
constexpr double kNoteDecay = 3.6;
constexpr double kStrikeDecay = 6.5;
constexpr double kTailPadding = 0.5;

// The bell timbre. Inharmonic on purpose: the hum an octave below and the
// tierce at 1.19 -- a minor third -- are what make this a bell rather than an
// organ, and the minor third is why bells sound faintly mournful.
struct Partial {
    double ratio;
    double amp;
    double decay;
};
const Partial kPartials[] = {
    {0.50, 0.35, 4.0}, {1.00, 1.00, 3.2}, {1.19, 0.55, 2.4}, {1.50, 0.32, 1.9},
    {2.00, 0.42, 1.6}, {2.51, 0.18, 1.1}, {3.01, 0.14, 0.9}, {4.10, 0.08, 0.6},
};

const int* ChangesFor(Quarter quarter, int* count) {
    static const int kPast[] = {0};
    static const int kHalf[] = {1, 2};
    static const int kTo[] = {3, 4, 0};
    static const int kHour[] = {1, 2, 3, 4};

    switch (quarter) {
        case Quarter::Past: *count = 1; return kPast;
        case Quarter::Half: *count = 2; return kHalf;
        case Quarter::To:   *count = 3; return kTo;
        case Quarter::Hour: default: *count = 4; return kHour;
    }
}

// ---------------------------------------------------------------- synthesis

struct Note {
    double startSec;
    double freq;
    double length;
};

// Mixes one struck note additively into `buf`.
//
// The spec's formula is evaluated per sample as
//   value = sum(amp * exp(-t / (decay * scale)) * sin(2*pi*freq*ratio*t))
// but computing a sin() and an exp() per partial per sample is roughly twenty
// million transcendental calls for a full hour ring, which is a visible stall on
// the UI thread. Both functions are being sampled on a uniform grid, so both
// have an exact one-step recurrence: the sine by rotating a unit vector through
// a fixed angle, the envelope by multiplying by a fixed ratio. The arithmetic
// is the same arithmetic, just carried forward instead of recomputed.
void MixNote(std::vector<float>* buf, const Note& note) {
    const double rate = kSampleRate;
    const size_t first = static_cast<size_t>(note.startSec * rate);
    if (first >= buf->size()) return;

    size_t count = static_cast<size_t>(note.length * rate);
    if (first + count > buf->size()) count = buf->size() - first;

    const double scale = note.length / kNoteDecay;

    for (const Partial& p : kPartials) {
        const double step = 2.0 * kPi * note.freq * p.ratio / rate;
        const double cosStep = std::cos(step);
        const double sinStep = std::sin(step);
        const double decayStep = std::exp(-1.0 / (rate * p.decay * scale));

        double sinT = 0.0;   // sin(n * step)
        double cosT = 1.0;   // cos(n * step)
        double env = p.amp;  // amp * exp(-t / (decay * scale))

        for (size_t i = 0; i < count; ++i) {
            const double t = static_cast<double>(i) / rate;
            // Four milliseconds of linear fade-in. Without it every note opens
            // on a step discontinuity, which is audible as a click.
            const double attack = (t < 0.004) ? (t / 0.004) : 1.0;

            // The attack and the 0.16 trim are linear, so applying them per
            // partial and summing is identical to applying them to the sum.
            (*buf)[first + i] += static_cast<float>(env * sinT * attack * 0.16);

            const double nextSin = sinT * cosStep + cosT * sinStep;
            cosT = cosT * cosStep - sinT * sinStep;
            sinT = nextSin;
            env *= decayStep;
        }
    }
}

std::vector<int16_t> Render(Quarter quarter, int hour12) {
    if (hour12 < 1) hour12 = 12;
    if (hour12 > 12) hour12 = 12;

    const Settings& cfg = Cfg();
    const bool strikes = (quarter == Quarter::Hour) && cfg.chimeStrikesHour;

    std::vector<Note> notes;
    double cursor = 0.0;

    int changeCount = 0;
    const int* changes = ChangesFor(quarter, &changeCount);
    for (int c = 0; c < changeCount; ++c) {
        for (int n = 0; n < 4; ++n) {
            notes.push_back({cursor, kChanges[changes[c]][n], kNoteDecay});
            cursor += kNoteGap;
        }
        cursor += kChangeGap;
    }

    if (strikes) {
        cursor += kBeforeStrikes;
        for (int i = 0; i < hour12; ++i) {
            notes.push_back({cursor, kHourBell, kStrikeDecay});
            cursor += kStrikeGap;
        }
    }

    // Per the spec, always the long decay plus half a second, whether or not
    // the hour was struck. A quarter therefore carries a little more silence
    // than it strictly needs, which costs nothing and keeps the one rule.
    const double totalSec = cursor + kStrikeDecay + kTailPadding;
    const size_t totalSamples = static_cast<size_t>(totalSec * kSampleRate);
    if (totalSamples == 0) return std::vector<int16_t>();

    std::vector<float> mix(totalSamples, 0.0f);
    for (const Note& n : notes) MixNote(&mix, n);

    // Volume is applied here rather than through waveOutSetVolume, which is a
    // per-device setting and would change the level of everything else the user
    // is listening to.
    const double volume = cfg.chimeVolume;

    std::vector<int16_t> pcm(totalSamples);
    for (size_t i = 0; i < totalSamples; ++i) {
        double v = static_cast<double>(mix[i]) * volume * 32767.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        pcm[i] = static_cast<int16_t>(v);
    }
    return pcm;
}

// ---------------------------------------------------------------- the device

struct Device {
    HWAVEOUT handle = nullptr;
    WAVEHDR header{};
    std::vector<int16_t> pcm;   // must outlive the write; waveOut does not copy
    bool playing = false;
    bool finished = false;      // raised by the callback, acted on by Tick
};

Device g_device;
Lock g_lock;

// The absolute quarter index, floor(epoch / 900), of the last quarter this
// process has consumed. INT64_MIN so the first eligible tick after launch rings.
Seconds g_lastQuarter = INT64_MIN;

void CALLBACK WaveProc(HWAVEOUT, UINT message, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (message != WOM_DONE) return;
    // This runs on a system thread inside the audio driver's context. The
    // documented rule is that a waveOut callback may not call any system
    // function except waveOutWrite, PostMessage, the timeGetTime family and
    // EnterCriticalSection / LeaveCriticalSection. Calling waveOutClose or
    // waveOutUnprepareHeader from here deadlocks. So all it does is raise a
    // flag; Tick, on the UI thread, does the teardown one second later.
    Guard guard(g_lock);
    g_device.finished = true;
}

void CloseDevice() {
    HWAVEOUT handle = nullptr;
    {
        Guard guard(g_lock);
        handle = g_device.handle;
        g_device.handle = nullptr;
        g_device.playing = false;
        g_device.finished = false;
    }
    if (!handle) return;

    // Reset first: it stops playback and returns the buffer, which is what
    // makes it safe to unprepare and then release the memory it points at.
    ::waveOutReset(handle);
    ::waveOutUnprepareHeader(handle, &g_device.header, sizeof(WAVEHDR));
    ::waveOutClose(handle);

    g_device.header = WAVEHDR{};
    g_device.pcm.clear();
    g_device.pcm.shrink_to_fit();
}

// Floor division, so a pre-1970 simulated clock does not round the wrong way
// and ring the same quarter twice.
Seconds QuarterIndex(Seconds epoch) {
    return (epoch >= 0) ? (epoch / 900) : -((899 - epoch) / 900);
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

void Init() {
    Guard guard(g_lock);
    g_lastQuarter = INT64_MIN;
}

void Shutdown() { CloseDevice(); }

bool IsRinging() {
    Guard guard(g_lock);
    return g_device.playing;
}

void Stop() { CloseDevice(); }

void OnDeviceChanged() {
    // Headphones, Bluetooth, a dock. The open handle refers to a device that
    // may no longer exist; dropping it means the next ring opens a fresh one
    // against whatever the default has become.
    CloseDevice();
}

// ---------------------------------------------------------------- ringing

void Ring(Quarter quarter, int hour12) {
    // Deliberately ignores both the schedule and Sound Hours: this is what the
    // "Hear It" menu is for, and a preview that refuses to preview is useless.
    CloseDevice();

    std::vector<int16_t> pcm = Render(quarter, hour12);
    if (pcm.empty()) return;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(kSampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    HWAVEOUT handle = nullptr;
    if (::waveOutOpen(&handle, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(WaveProc), 0,
                      CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        return;   // no output device, or every one of them is in exclusive use
    }

    {
        Guard guard(g_lock);
        g_device.pcm = std::move(pcm);
        g_device.header = WAVEHDR{};
        g_device.header.lpData = reinterpret_cast<LPSTR>(g_device.pcm.data());
        g_device.header.dwBufferLength =
            static_cast<DWORD>(g_device.pcm.size() * sizeof(int16_t));
        g_device.handle = handle;
        g_device.playing = true;
        g_device.finished = false;
    }

    if (::waveOutPrepareHeader(handle, &g_device.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
        ::waveOutWrite(handle, &g_device.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        CloseDevice();
    }
}

void Tick(Seconds now, const TimeZone& zone) {
    // Teardown happens here rather than in the callback, which is not allowed
    // to close anything. Belt and braces: the header's own DONE bit is checked
    // as well, in case a driver never delivers WOM_DONE.
    bool shouldClose = false;
    {
        Guard guard(g_lock);
        if (g_device.playing &&
            (g_device.finished || (g_device.header.dwFlags & WHDR_DONE) != 0)) {
            shouldClose = true;
        }
    }
    if (shouldClose) CloseDevice();

    const Settings& cfg = Cfg();
    if (cfg.chimeMode == ChimeMode::Off) return;

    const TimeZone::Parts p = zone.Break(now);
    if (p.minute % 15 != 0) return;

    Quarter quarter;
    switch (p.minute) {
        case 0:  quarter = Quarter::Hour; break;
        case 15: quarter = Quarter::Past; break;
        case 30: quarter = Quarter::Half; break;
        default: quarter = Quarter::To; break;
    }
    if (cfg.chimeMode == ChimeMode::Hourly && quarter != Quarter::Hour) return;

    // Absolute rather than wall-clock. At a DST fall-back the same wall-clock
    // hour happens twice; a wall-clock index would treat the second one as
    // already rung and silence it.
    const Seconds index = QuarterIndex(now);
    if (index == g_lastQuarter) return;

    // Stamped before the five-second check, deliberately. A tick that arrives
    // late -- the machine was asleep, or the app was busy -- consumes that
    // quarter permanently. Waking a machine at twenty past must not ring the
    // quarter it slept through, and a chime that plays at the wrong minute is
    // worse than no chime at all.
    g_lastQuarter = index;
    if (p.second > 5) return;

    if (!soundhours::Allows(now, zone)) return;

    const int hour12 = (p.hour % 12 == 0) ? 12 : (p.hour % 12);
    Ring(quarter, hour12);
}

std::wstring DescribeMode(ChimeMode mode) {
    switch (mode) {
        case ChimeMode::Hourly:    return L"On the hour";
        case ChimeMode::Quarterly: return L"Every quarter hour";
        case ChimeMode::Off:
        default:                   return L"Off";
    }
}

}  // namespace westminster
}  // namespace rc
