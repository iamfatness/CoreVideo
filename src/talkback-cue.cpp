// src/talkback-cue.cpp
// Plays the talkback open/close audio cue on the Windows default playback
// device.
//
// WHY PlaySound(SND_MEMORY | SND_ASYNC), not WASAPI/IMMDeviceEnumerator or a
// third-party mixer: it targets the system default output with no device
// enumeration, needs no COM initialisation, and SND_ASYNC returns as soon as
// playback starts. The no-COM property specifically matters here: this file
// runs its playback on a throwaway std::thread (see talkback_play_cue()
// below) that this plugin never calls CoInitialize on, and WASAPI requires
// COM on whatever thread touches it. PlaySound needs neither, and pulls in
// no new third-party dependency -- it's part of winmm, which ships with
// Windows.
//
// WHY THIS FILE NEVER TOUCHES libobs AUDIO: see talkback-isolation-test.cpp
// (the tap's version) and its sibling for this file, both enforced by a
// build-time source scan, not just this comment -- a later "route the cue
// through OBS so it lands on the monitor mix" change must fail the build,
// not quietly put beeps on air.
//
// WHAT HAPPENS WHEN A CUE IS REQUESTED WHILE ONE IS PLAYING: REPLACE, not
// drop or queue. PlaySound's own documented behaviour is that only one
// async sound plays per process at a time -- a new SND_ASYNC call stops
// whatever is currently sounding before starting the new one. That is not
// something this file implements; it falls out of calling PlaySoundA a
// second time. It is the right choice here regardless of which API had
// implemented it: a CLOSE requested while OPEN is still sounding (a very
// quick key tap) must still be audible -- silently dropping it would leave
// the operator believing they're still keyed when they are not, which is
// the exact failure this feature exists to prevent.
#include "talkback-cue.h"
#include "talkback-tone.h"

#if defined(WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// Matches the probe tone's sample rate (talkback-tone.h / engine-talkback);
// no reason to differ, and matching keeps the ear's reference point (the
// probe's tone quality) the same one the cue reuses.
constexpr uint32_t kSampleRate = 48000;
// Below 1.0 so talkback_tone_fill()'s int16 conversion never wraps on
// rounding -- see that function's own doc comment.
constexpr double kAmplitude = 0.5;

struct CueSpec {
    double   freq_hz;
    uint32_t duration_ms;
};

// OPEN: ~880 Hz / ~120 ms. CLOSE: ~440 Hz / ~180 ms. High-then-low is the
// broadcast convention (open = attention rising, close = stand-down) and
// stays distinguishable from program/monitor audio bleeding into a busy
// headset -- see the spec this file implements.
CueSpec spec_for(TalkbackCue cue)
{
    switch (cue) {
    case TalkbackCue::Open:
        return {880.0, 120};
    case TalkbackCue::Close:
        return {440.0, 180};
    case TalkbackCue::None:
        break;
    }
    return {0.0, 0};
}

// Minimal mono 16-bit PCM WAV header. PlaySound(SND_MEMORY) wants a real WAV
// image in memory, not a raw sample buffer -- it parses the RIFF/fmt/data
// chunks itself.
#pragma pack(push, 1)
struct WavHeader {
    char     riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_size = 0;
    char     wave[4] = {'W', 'A', 'V', 'E'};
    char     fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 1;
    uint32_t sample_rate = kSampleRate;
    uint32_t byte_rate = kSampleRate * sizeof(int16_t);
    uint16_t block_align = sizeof(int16_t);
    uint16_t bits_per_sample = 16;
    char     data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)

// Builds the cue's WAV image, generated with the EXISTING
// talkback_tone_fill() the Milestone 1 probe tone uses (src/talkback-tone.h)
// -- this file must not grow a second tone generator; see that header's own
// doc comment for why a generated sine beats a shipped asset, which applies
// identically here.
std::vector<uint8_t> build_wav(TalkbackCue cue)
{
    const CueSpec spec = spec_for(cue);
    const std::size_t sample_count =
        static_cast<std::size_t>(kSampleRate) * spec.duration_ms / 1000;

    std::vector<int16_t> samples(sample_count);
    talkback_tone_fill(samples.data(), sample_count, /*start_index=*/0,
                        kSampleRate, spec.freq_hz, kAmplitude);

    const uint32_t data_bytes =
        static_cast<uint32_t>(sample_count * sizeof(int16_t));

    WavHeader header;
    header.data_size = data_bytes;
    header.riff_size = static_cast<uint32_t>(sizeof(WavHeader) - 8 + data_bytes);

    std::vector<uint8_t> wav(sizeof(WavHeader) + data_bytes);
    std::memcpy(wav.data(), &header, sizeof(WavHeader));
    if (data_bytes > 0)
        std::memcpy(wav.data() + sizeof(WavHeader), samples.data(), data_bytes);
    return wav;
}

} // namespace

void talkback_play_cue(TalkbackCue cue)
{
    if (cue == TalkbackCue::None) return;

    const uint32_t duration_ms = spec_for(cue).duration_ms;

    // Fire-and-forget on a short-lived, DETACHED worker thread -- never the
    // caller's thread. See talkback-cue.h's doc comment on talkback_play_cue
    // for why: evaluate() runs on the Qt main thread every 25ms, and
    // key_off() can run there too. A blocking sound call on either stalls
    // the whole OBS UI for the cue's duration. The thread owns its own WAV
    // buffer (built here, not passed in) so nothing about its lifetime
    // depends on the caller's stack.
    std::thread([cue, duration_ms] {
        const std::vector<uint8_t> wav = build_wav(cue);
        if (wav.empty()) return;

        // SND_MEMORY: `wav` IS the sound image, not a filename. SND_ASYNC:
        // returns as soon as playback starts -- see the file comment above
        // for why this is also this file's whole REPLACE-on-overlap
        // strategy. SND_NODEFAULT: if playback can't start (e.g. no output
        // device), stay silent rather than fall back to Windows' own system
        // sound, which would be a more confusing signal than no cue.
        PlaySoundA(reinterpret_cast<LPCSTR>(wav.data()), nullptr,
                   SND_MEMORY | SND_ASYNC | SND_NODEFAULT);

        // SND_MEMORY playback reads directly from `wav` -- winmm does not
        // copy it. Keep the buffer alive (i.e. keep this thread alive) for
        // the cue's duration plus a margin, so playback never reads freed
        // memory. If a LATER call has already replaced this sound by the
        // time this sleep ends, that replacement has already stopped this
        // buffer being read (see the file comment above); freeing here is
        // then just reclaiming memory nothing references any more.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(duration_ms + 50));
    }).detach();
}

#else // !WIN32

void talkback_play_cue(TalkbackCue)
{
    // No playback path on this platform yet. Silently doing nothing (rather
    // than failing to build) matches how the rest of this plugin treats
    // Windows-only pieces during the mac port -- see zoom-meeting.cpp's
    // `#if defined(WIN32)` guard for the same pattern.
}

#endif
