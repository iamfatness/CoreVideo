// tests/talkback-tone-test.cpp
// The probe's test tone. Pinned because a phase discontinuity between buffers
// is audible as a click, and a listener reporting "clicky audio" would be
// mistaken for a transport fault when the transport was fine.
#include "talkback-tone.h"

#include <cmath>
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
    constexpr uint32_t kRate = 48000;
    constexpr double kFreq = 440.0;
    constexpr double kAmp = 0.5;

    // ── Returns the next absolute index, so callers can chain ──
    std::vector<int16_t> a(480);
    const uint64_t next = talkback_tone_fill(a.data(), a.size(), 0, kRate, kFreq, kAmp);
    check(next == 480, "talkback_tone_fill did not return start_index + count");

    // ── Phase is continuous across a buffer boundary ──
    // Filling [0,960) in one call must equal filling [0,480) then [480,960).
    std::vector<int16_t> whole(960);
    talkback_tone_fill(whole.data(), whole.size(), 0, kRate, kFreq, kAmp);
    std::vector<int16_t> second(480);
    talkback_tone_fill(second.data(), second.size(), next, kRate, kFreq, kAmp);
    bool contiguous = true;
    for (size_t i = 0; i < 480; ++i) {
        if (whole[i] != a[i] || whole[480 + i] != second[i]) { contiguous = false; break; }
    }
    check(contiguous,
          "phase restarted at the buffer boundary -- a chained fill did not match "
          "one continuous fill, which is an audible click every buffer");

    // ── Amplitude is respected and never clips ──
    int16_t peak = 0;
    for (int16_t s : whole) {
        const int16_t mag = static_cast<int16_t>(s < 0 ? -s : s);
        if (mag > peak) peak = mag;
    }
    check(peak <= 16384 + 64, "tone exceeded the requested 0.5 amplitude");
    check(peak > 16384 - 512, "tone was far quieter than the requested amplitude");

    // ── Starts at zero, so keying in does not begin with a step ──
    check(whole[0] == 0, "the tone did not start at zero amplitude");

    // ── One full period lands back near zero (440Hz at 48kHz ~ 109.09 samples) ──
    std::vector<int16_t> one_sec(kRate);
    talkback_tone_fill(one_sec.data(), one_sec.size(), 0, kRate, kFreq, kAmp);
    int zero_crossings = 0;
    for (size_t i = 1; i < one_sec.size(); ++i) {
        if ((one_sec[i - 1] < 0) != (one_sec[i] < 0)) ++zero_crossings;
    }
    // 440Hz => 880 sign changes per second, allow a couple either side.
    check(zero_crossings >= 878 && zero_crossings <= 882,
          "the tone was not 440Hz -- zero-crossing count was wrong");

    // ── Zero count is a no-op, not a crash ──
    const uint64_t same = talkback_tone_fill(nullptr, 0, 1234, kRate, kFreq, kAmp);
    check(same == 1234, "a zero-length fill did not return start_index unchanged");

    if (failures == 0)
        std::cout << "talkback-tone: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
