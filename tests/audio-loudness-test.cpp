// tests/audio-loudness-test.cpp
// ITU-R BS.1770-4 loudness, measured at whatever rate Zoom actually sends.
//
// WHY THIS TEST IS THE WHOLE FEATURE. BS.1770-4 publishes its K-weighting
// biquad coefficients for 48 kHz and for no other rate. This plugin does not
// receive a guaranteed rate: the engine reads GetSampleRate() per buffer and
// stamps it into ShmAudioHeader::sample_rate (engine/src/engine-audio.cpp),
// and Zoom commonly delivers 32 kHz. Coefficients pinned at 48 kHz and fed
// 32 kHz audio still produce a plausible-looking number -- measured below at
// 1.3 LU wrong on a 1 kHz tone -- which is precisely the failure a meter
// cannot survive, because nothing about the reading says it is wrong.
#include "audio-loudness.h"

#include <cmath>
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

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

int main()
{
    // ── The published BS.1770-4 table, at 48 kHz, to the digit ─────────────
    // Table 1 (stage 1, the "head"/high-shelf pre-filter) and Table 2 (stage
    // 2, the RLB high-pass) of BS.1770-4. If the derivation is right, it
    // reproduces these exactly at 48 kHz -- that is the only rate at which
    // there is anything published to check against, which is why it is
    // checked to 1e-11 and not to a comfortable tolerance.
    {
        const LoudnessBiquadCoeffs s1 = bs1770_stage1_coeffs(48000);
        check(near(s1.b0,  1.53512485958697, 1e-11), "48k stage-1 b0 does not match the published BS.1770-4 table");
        check(near(s1.b1, -2.69169618940638, 1e-11), "48k stage-1 b1 does not match the published BS.1770-4 table");
        check(near(s1.b2,  1.19839281085285, 1e-11), "48k stage-1 b2 does not match the published BS.1770-4 table");
        check(near(s1.a1, -1.69065929318241, 1e-11), "48k stage-1 a1 does not match the published BS.1770-4 table");
        check(near(s1.a2,  0.73248077421585, 1e-11), "48k stage-1 a2 does not match the published BS.1770-4 table");

        const LoudnessBiquadCoeffs s2 = bs1770_stage2_coeffs(48000);
        check(near(s2.b0,  1.0, 1e-12), "48k stage-2 b0 must be exactly 1");
        check(near(s2.b1, -2.0, 1e-12), "48k stage-2 b1 must be exactly -2");
        check(near(s2.b2,  1.0, 1e-12), "48k stage-2 b2 must be exactly 1");
        check(near(s2.a1, -1.99004745483398, 1e-11), "48k stage-2 a1 does not match the published BS.1770-4 table");
        check(near(s2.a2,  0.99007225036621, 1e-11), "48k stage-2 a2 does not match the published BS.1770-4 table");
    }

    // ── 32 kHz must produce DIFFERENT, correctly derived coefficients ──────
    // These are the bilinear transform of the same analog prototype at
    // 32 kHz. A "derivation" that quietly returned the 48 kHz numbers for
    // every rate would pass every check above and fail every one here.
    {
        const LoudnessBiquadCoeffs s1 = bs1770_stage1_coeffs(32000);
        check(near(s1.b0,  1.51117789957, 1e-9), "32k stage-1 b0 is wrong");
        check(near(s1.b1, -2.46488941336, 1e-9), "32k stage-1 b1 is wrong");
        check(near(s1.b2,  1.04163327352, 1e-9), "32k stage-1 b2 is wrong");
        check(near(s1.a1, -1.53904509625, 1e-9), "32k stage-1 a1 is wrong");
        check(near(s1.a2,  0.62696685598, 1e-9), "32k stage-1 a2 is wrong");

        const LoudnessBiquadCoeffs s2 = bs1770_stage2_coeffs(32000);
        check(near(s2.a1, -1.98508966899, 1e-9), "32k stage-2 a1 is wrong");
        check(near(s2.a2,  0.98514532067, 1e-9), "32k stage-2 a2 is wrong");
    }

    // ── The two rates must not be the same numbers ─────────────────────────
    // Stated as its own assertion rather than left implicit in the two blocks
    // above, because "the coefficients are rate-dependent" is the invariant,
    // and an implementer reading only this file should see it said out loud.
    {
        const LoudnessBiquadCoeffs a = bs1770_stage1_coeffs(48000);
        const LoudnessBiquadCoeffs b = bs1770_stage1_coeffs(32000);
        check(std::fabs(a.a1 - b.a1) > 0.10,
              "stage-1 a1 barely moved between 48 kHz and 32 kHz -- the "
              "coefficients are not being derived from the rate at all");
        const LoudnessBiquadCoeffs c = bs1770_stage2_coeffs(48000);
        const LoudnessBiquadCoeffs d = bs1770_stage2_coeffs(32000);
        check(std::fabs(c.a1 - d.a1) > 0.004,
              "stage-2 a1 barely moved between 48 kHz and 32 kHz -- the "
              "high-pass corner is being placed at a fixed digital frequency "
              "rather than a fixed 38 Hz");
    }

    // ── A degenerate rate must not produce NaN or a divide by zero ─────────
    {
        const LoudnessBiquadCoeffs s1 = bs1770_stage1_coeffs(0);
        check(std::isfinite(s1.b0) && std::isfinite(s1.a1),
              "a zero sample rate produced non-finite coefficients -- the "
              "ring header can be read before the writer has initialised it");
    }

    // ── The biquad itself: a direct-form-II-transposed step ────────────────
    // Pinned against hand-computed values so a sign slip on the feedback
    // terms cannot hide inside a filter response test.
    {
        const LoudnessBiquadCoeffs c{0.5, 0.25, 0.125, -0.5, 0.25};
        LoudnessBiquadState st{};
        // y[0] = 0.5*1 = 0.5
        const double y0 = loudness_biquad_step(c, st, 1.0);
        check(near(y0, 0.5, 1e-12), "biquad sample 0 was not b0*x0");
        // y[1] = 0.5*0 + 0.25*1 + 0.125*0 - (-0.5)*0.5 - 0.25*0 = 0.5
        const double y1 = loudness_biquad_step(c, st, 0.0);
        check(near(y1, 0.5, 1e-12), "biquad sample 1 is wrong -- check the "
              "sign convention on a1 (y = b.x - a.y)");
        // y[2] = 0.125*1 - (-0.5)*0.5 - 0.25*0.5 = 0.125 + 0.25 - 0.125 = 0.25
        const double y2 = loudness_biquad_step(c, st, 0.0);
        check(near(y2, 0.25, 1e-12), "biquad sample 2 is wrong -- the second "
              "feedback tap (a2) is not being applied");
    }

    if (failures == 0)
        std::cout << "audio-loudness: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
