// tests/talkback-pcm-test.cpp
// libobs planar float -> Zoom interleaved int16.
//
// Pinned hard because every failure mode here is SILENT: a channel-order slip,
// a missing clamp, or a wrong scale factor produces noise or silence rather
// than an error, and the first report would come from a person on air saying
// "talkback sounds broken".
#include "talkback-pcm.h"

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
    // ── Byte sizing ────────────────────────────────────────────────────────
    check(talkback_pcm_bytes(480, 1) == 960, "mono 480 frames was not 960 bytes");
    check(talkback_pcm_bytes(480, 2) == 1920, "stereo 480 frames was not 1920 bytes");
    check((talkback_pcm_bytes(481, 2) % 2) == 0,
          "byte length was odd -- SendAudioDataToChannel requires a multiple of 2");

    // ── Interleaving puts channels in the right order ──────────────────────
    const float lp[4] = {0.0f,  0.5f, -0.5f, 1.0f};
    const float rp[4] = {0.25f, 0.0f, -1.0f, 0.0f};
    const float *planes2[2] = {lp, rp};
    std::vector<int16_t> out(8);
    talkback_pcm_interleave(planes2, 4, 2, out.data());
    check(out[0] == 0, "frame 0 left was not silence");
    check(out[1] > 8000 && out[1] < 8400, "frame 0 right (0.25) was out of range");
    check(out[2] > 16000 && out[2] < 16500, "frame 1 left (0.5) was out of range");
    check(out[3] == 0, "frame 1 right was not silence");
    check(out[4] < -16000 && out[4] > -16500, "frame 2 left (-0.5) was out of range");

    // ── Full scale must not wrap ───────────────────────────────────────────
    check(out[6] == 32767, "+1.0 did not clamp to INT16_MAX");
    const float minus[1] = {-1.0f};
    const float *planes1[1] = {minus};
    int16_t one = 0;
    talkback_pcm_interleave(planes1, 1, 1, &one);
    check(one == -32767 || one == -32768, "-1.0 did not map to full negative scale");

    // ── Out-of-range input clamps rather than wrapping ─────────────────────
    // A source with gain above unity WILL exceed +/-1.0. Wrapping turns a loud
    // passage into a full-scale square wave: the single worst sound to put in
    // a director's ear.
    const float hot[4] = {2.0f, -2.0f, 9.9f, -9.9f};
    const float *hotp[1] = {hot};
    std::vector<int16_t> hotout(4);
    talkback_pcm_interleave(hotp, 4, 1, hotout.data());
    check(hotout[0] == 32767, "+2.0 did not clamp");
    check(hotout[1] <= -32767, "-2.0 did not clamp");
    check(hotout[2] == 32767, "+9.9 did not clamp");
    check(hotout[3] <= -32767, "-9.9 did not clamp");

    // ── NaN/Inf must not become random noise ───────────────────────────────
    const float bad[2] = {std::nanf(""), INFINITY};
    const float *badp[1] = {bad};
    std::vector<int16_t> badout(2);
    talkback_pcm_interleave(badp, 2, 1, badout.data());
    check(badout[0] == 0, "NaN did not become silence");
    check(badout[1] == 32767, "+Inf did not clamp to full scale");

    // ── Rate gate matches what the SDK documents ───────────────────────────
    check(talkback_pcm_rate_supported(48000), "48000 was rejected");
    check(talkback_pcm_rate_supported(32000), "32000 was rejected");
    check(talkback_pcm_rate_supported(44100), "44100 was rejected");
    check(!talkback_pcm_rate_supported(0),     "0 was accepted");
    check(!talkback_pcm_rate_supported(22050), "22050 was accepted");

    // ── Degenerate input is a no-op, not a crash ───────────────────────────
    talkback_pcm_interleave(nullptr, 4, 1, out.data());
    talkback_pcm_interleave(planes2, 0, 2, out.data());
    talkback_pcm_interleave(planes2, 4, 0, out.data());
    talkback_pcm_interleave(planes2, 4, 2, nullptr);
    check(true, "degenerate input crashed");

    if (failures == 0)
        std::cout << "talkback-pcm: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
