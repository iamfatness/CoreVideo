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

    // ── Reference tones: the numbers an implementer can check by hand ──────
    //
    // The K-weighting curve has a gain of exactly +0.691 dB at 997 Hz, and
    // BS.1770's -0.691 dB offset is there to cancel it. So for a ~1 kHz sine
    // the whole measurement collapses to L = 10*log10(mean square of the
    // un-weighted signal), which is a number that can be worked out on paper:
    //
    //   peak 1.0        -> mean square 0.5    -> -3.01 LUFS
    //   peak 0.1        -> mean square 0.005  -> -23.01 LUFS
    //   RMS  0.1        -> mean square 0.01   -> -20.00 LUFS
    //
    // The third is the one to remember: a 1 kHz tone at -20 dBFS RMS reads
    // -20.0 LUFS. If that does not hold, the offset, the channel weight, the
    // int16 scaling or the K-weighting is wrong, and no amount of relative
    // comparison downstream will save the reading.
    auto feed_sine = [](LoudnessMeter &m, uint32_t rate, double peak,
                        double freq, double seconds) {
        const size_t n = static_cast<size_t>(rate * seconds);
        std::vector<int16_t> pcm(n);
        for (size_t i = 0; i < n; ++i) {
            const double v = peak * std::sin(2.0 * 3.14159265358979323846 *
                                             freq * static_cast<double>(i) /
                                             static_cast<double>(rate));
            double s = v * 32767.0;
            if (s > 32767.0)  s =  32767.0;
            if (s < -32767.0) s = -32767.0;
            pcm[i] = static_cast<int16_t>(std::lround(s));
        }
        loudness_meter_feed_int16(m, pcm.data(), n, 1, rate);
    };

    {
        LoudnessMeter m;
        feed_sine(m, 48000, 1.0, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs),
              "momentary loudness was unavailable after 5 s of tone");
        check(near(lufs, -3.01, 0.10),
              "a full-scale 1 kHz sine at 48 kHz did not read -3.01 LUFS");
    }
    {
        LoudnessMeter m;
        feed_sine(m, 48000, 0.1, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs), "momentary unavailable");
        check(near(lufs, -23.01, 0.10),
              "a 1 kHz sine of peak amplitude 0.1 at 48 kHz did not read "
              "-23.01 LUFS");
    }
    {
        // -20 dBFS RMS: peak = sqrt(2) * 0.1.
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs), "momentary unavailable");
        check(near(lufs, -20.00, 0.10),
              "a -20 dBFS RMS 1 kHz sine at 48 kHz did not read -20.0 LUFS -- "
              "K-weighting is ~0 dB at 1 kHz once the -0.691 offset is "
              "applied, so this is an equality, not an approximation");
    }

    // ── The same tone at 32 kHz must read the same, not 1.3 LU high ────────
    // This is the assertion the whole runtime-rate design exists for. With
    // the 48 kHz coefficients applied to 32 kHz audio this tone reads
    // -18.66 LUFS instead of -19.98: it passes a "looks like a plausible
    // loudness" eyeball test and fails here.
    {
        LoudnessMeter m;
        feed_sine(m, 32000, std::sqrt(2.0) * 0.1, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs), "momentary unavailable at 32 kHz");
        check(near(lufs, -19.98, 0.12),
              "a -20 dBFS RMS 1 kHz sine at 32 kHz did not read -20 LUFS -- "
              "the coefficients are not following the runtime rate");
        check(lufs < -19.5,
              "the 32 kHz reading is more than 0.5 LU hot, which is the "
              "signature of 48 kHz coefficients being used at 32 kHz");
    }

    // ── Short-term needs 3 s; momentary needs 400 ms ───────────────────────
    {
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 0.35);
        double lufs = 0.0;
        check(!loudness_meter_momentary(m, &lufs),
              "momentary reported a value before a full 400 ms block existed");
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 0.20);
        check(loudness_meter_momentary(m, &lufs),
              "momentary was still unavailable after 550 ms");
        check(!loudness_meter_short_term(m, &lufs),
              "short-term reported a value before 3 s of audio existed");
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 3.0);
        check(loudness_meter_short_term(m, &lufs),
              "short-term was still unavailable after 3.5 s");
        check(near(lufs, -20.00, 0.15), "short-term did not read -20 LUFS");
    }

    // ── Stereo: two identical channels are +3 dB, not the same as mono ─────
    // BS.1770 sums the weighted per-channel mean squares (G = 1.0 for L and
    // R), it does not average them. Averaging is the mistake that makes a
    // stereo panelist read 3 LU quieter than the identical mono one beside
    // them, which is exactly the comparison this feature exists to make.
    {
        LoudnessMeter mono;
        feed_sine(mono, 48000, std::sqrt(2.0) * 0.1, 1000.0, 2.0);
        double mono_lufs = 0.0;
        check(loudness_meter_momentary(mono, &mono_lufs), "mono unavailable");

        LoudnessMeter st;
        const size_t n = 48000 * 2;
        std::vector<int16_t> pcm(n * 2);
        for (size_t i = 0; i < n; ++i) {
            const double v = std::sqrt(2.0) * 0.1 *
                std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                         static_cast<double>(i) / 48000.0);
            const int16_t s = static_cast<int16_t>(std::lround(v * 32767.0));
            pcm[i * 2]     = s;
            pcm[i * 2 + 1] = s;
        }
        loudness_meter_feed_int16(st, pcm.data(), n, 2, 48000);
        double st_lufs = 0.0;
        check(loudness_meter_momentary(st, &st_lufs), "stereo unavailable");
        check(near(st_lufs - mono_lufs, 3.01, 0.05),
              "dual-mono stereo was not +3.01 LU relative to mono -- the "
              "channels are being averaged instead of summed");
    }

    // ── Digital silence never produces NaN or -inf leaking to a caller ─────
    {
        LoudnessMeter m;
        std::vector<int16_t> zeros(48000, 0);
        loudness_meter_feed_int16(m, zeros.data(), zeros.size(), 1, 48000);
        double lufs = 0.0;
        const bool have = loudness_meter_momentary(m, &lufs);
        check(!have || std::isfinite(lufs),
              "true digital silence produced a non-finite momentary reading -- "
              "a panelist who has not spoken yet is the normal case here, not "
              "an edge case");
    }

    if (failures == 0)
        std::cout << "audio-loudness: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
