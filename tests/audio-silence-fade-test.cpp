// tests/audio-silence-fade-test.cpp
// Ramping the first buffer published after true digital silence.
//
// The incident this guards (2026-08-18, live soak, "clicks and pops tied to
// the active speaker feed"). Probed straight from the engine's audio SHM
// ring, bypassing OBS and its gate entirely: the ring genuinely carries
// multi-hundred-millisecond runs of exact-zero PCM between a bot's spoken
// turns (measured 190-650 ms), then resumes at full amplitude with zero
// ramp. That is an instant 0 -> full-scale jump, which is exactly what an
// audible pop is made of. See src/audio-silence-fade.h for the full account.
#include "audio-silence-fade.h"

#include <cstdint>
#include <iostream>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    // --- Silence detection ---
    {
        std::vector<int16_t> zero(480, 0);
        check(audio_buffer_is_silent(zero.data(), zero.size()),
              "an all-zero buffer was not recognized as silent");

        std::vector<int16_t> one_nonzero(480, 0);
        one_nonzero[479] = 1; // last sample only
        check(!audio_buffer_is_silent(one_nonzero.data(), one_nonzero.size()),
              "a single nonzero sample at the end was missed -- must not "
              "early-out past real content");

        std::vector<int16_t> loud(480, 12000);
        check(!audio_buffer_is_silent(loud.data(), loud.size()),
              "ordinary speech was classified as silent");

        check(audio_buffer_is_silent(nullptr, 0),
              "an empty buffer must be silent (vacuously true) -- a 0-length "
              "read is not evidence of sound");
    }

    // --- Resume fade: the first frame is not slammed to zero ---
    {
        std::vector<int16_t> pcm(480, 20000); // full-scale-ish mono, 480 frames
        audio_apply_resume_fade(pcm.data(), 480, 1, /*ramp_frames=*/240);
        check(pcm[0] != 0,
              "the very first sample was zeroed -- that only moves the "
              "discontinuity to the buffer's own start, it does not remove it");
        check(pcm[0] < 20000,
              "the first sample was left at full scale -- no ramp happened");
    }

    // --- Resume fade: monotonically increasing gain across the ramp ---
    {
        std::vector<int16_t> pcm(480, 10000);
        audio_apply_resume_fade(pcm.data(), 480, 1, 240);
        for (int i = 1; i < 240; ++i) {
            check(pcm[i] >= pcm[i - 1],
                  "gain was not monotonically increasing across the ramp -- "
                  "that is itself an audible artifact");
        }
        check(pcm[239] == 10000,
              "the ramp did not reach full unity gain by its last frame");
    }

    // --- Resume fade: content past the ramp is untouched ---
    {
        std::vector<int16_t> pcm(480, 10000);
        audio_apply_resume_fade(pcm.data(), 480, 1, 240);
        check(pcm[240] == 10000 && pcm[479] == 10000,
              "samples past the ramp window were altered -- the fade must "
              "stay local to the onset, not recolor the rest of the buffer");
    }

    // --- Resume fade: stereo stays in phase (both channels scaled equally
    // per frame, not independently) ---
    {
        std::vector<int16_t> pcm = {10000, -10000, 10000, -10000,
                                    10000, -10000, 10000, -10000};
        audio_apply_resume_fade(pcm.data(), /*frames=*/4, /*channels=*/2,
                                /*ramp_frames=*/4);
        for (int f = 0; f < 4; ++f) {
            check(pcm[f * 2] == -pcm[f * 2 + 1],
                  "left/right gain diverged mid-ramp -- a mono source panned "
                  "to stereo would smear across the stereo field");
        }
    }

    // --- Resume fade: a buffer shorter than the requested ramp fades in
    // full rather than doing nothing ---
    {
        std::vector<int16_t> pcm(100, 8000);
        audio_apply_resume_fade(pcm.data(), 100, 1, /*ramp_frames=*/240);
        check(pcm[0] != 0 && pcm[0] < 8000,
              "a buffer shorter than the ramp window was not faded at all");
        check(pcm[99] == 8000,
              "a buffer shorter than the ramp window did not reach unity by "
              "its own last frame");
    }

    // --- Degenerate inputs never crash and never touch nonexistent data ---
    {
        std::vector<int16_t> pcm(10, 5000);
        audio_apply_resume_fade(pcm.data(), 0, 1, 240);
        audio_apply_resume_fade(pcm.data(), 10, 0, 240);
        audio_apply_resume_fade(pcm.data(), 10, 1, 0);
        check(pcm[0] == 5000,
              "a zero-frames/zero-channels/zero-ramp call mutated the buffer");
    }

    if (failures == 0)
        std::cout << "audio-silence-fade: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
