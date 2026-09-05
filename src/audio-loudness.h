#pragma once

// ITU-R BS.1770-4 loudness measurement, derived for the sample rate the audio
// ACTUALLY arrives at.
//
// WHY THIS FILE DERIVES INSTEAD OF QUOTING. BS.1770-4 tabulates its two
// K-weighting biquads' coefficients for 48 kHz and for no other rate. This
// plugin has no guaranteed rate: engine/src/engine-audio.cpp calls
// data->GetSampleRate() per buffer and stamps the answer into
// ShmAudioHeader::sample_rate, and Zoom commonly delivers 32 kHz. Applying
// the published 48 kHz numbers to 32 kHz audio moves both filters' corner
// frequencies by a factor of 1.5 and mis-weights every measurement: on a
// 1 kHz tone whose true value is -19.98 LUFS it reads -18.66 LUFS. That is
// 1.3 LU of error on a meter whose entire product claim is that a 6 LU
// spread between panelists is visible -- and nothing about the number looks
// wrong. So the coefficients come from the analog prototype in the standard,
// bilinear-transformed at the runtime rate. At 48 kHz the derivation
// reproduces the published table to fourteen digits, which is what
// tests/audio-loudness-test.cpp asserts.
//
// Pure by design -- no libobs, no Qt, no Zoom SDK -- so the whole measurement
// can be pinned against reference tones with no meeting, the same treatment
// audio-timeline.h and audio-silence-fade.h get, and for the same reason:
// the only symptom of a regression here is a number that is quietly wrong.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// One biquad section, y[n] = b0.x[n] + b1.x[n-1] + b2.x[n-2]
//                           - a1.y[n-1] - a2.y[n-2]
// (a0 normalised to 1). Sign convention matches the standard's tables, so a
// published a1 of -1.69065929318241 is stored verbatim.
struct LoudnessBiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct LoudnessBiquadState {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

inline double loudness_biquad_step(const LoudnessBiquadCoeffs &c,
                                   LoudnessBiquadState &s, double x)
{
    const double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
                     - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    s.y1 = y;
    return y;
}

// The analog prototype BS.1770-4's 48 kHz table was itself produced from.
// These five constants are the whole of the standard's filter specification
// once the rate is factored out; every published coefficient falls out of
// them. Kept at full precision because the 48 kHz reproduction is asserted to
// 1e-11.
constexpr double kBs1770Stage1Hz   = 1681.974450955533;
constexpr double kBs1770Stage1GdB  = 3.999843853973347;
constexpr double kBs1770Stage1Q    = 0.7071752369554196;
constexpr double kBs1770Stage1VbExp = 0.4996667741545416;
constexpr double kBs1770Stage2Hz   = 38.13547087602444;
constexpr double kBs1770Stage2Q    = 0.5003270373238773;

// A rate to fall back on when the caller hands us nothing usable. The ring
// header can legitimately be read before the writer has initialised it (see
// output_audio_frame()'s slot_count guard), and a zero rate must produce
// finite coefficients rather than a NaN that then poisons every subsequent
// filter state for the life of the source.
constexpr uint32_t kLoudnessFallbackRate = 48000;

inline uint32_t loudness_usable_rate(uint32_t sample_rate)
{
    return (sample_rate >= 8000 && sample_rate <= 384000)
               ? sample_rate : kLoudnessFallbackRate;
}

// Stage 1: the "head" high-shelf, roughly +4 dB above 1 kHz.
inline LoudnessBiquadCoeffs bs1770_stage1_coeffs(uint32_t sample_rate)
{
    const double fs = static_cast<double>(loudness_usable_rate(sample_rate));
    const double K  = std::tan(3.14159265358979323846 * kBs1770Stage1Hz / fs);
    const double Vh = std::pow(10.0, kBs1770Stage1GdB / 20.0);
    const double Vb = std::pow(Vh, kBs1770Stage1VbExp);
    const double a0 = 1.0 + K / kBs1770Stage1Q + K * K;

    LoudnessBiquadCoeffs c;
    c.b0 = (Vh + Vb * K / kBs1770Stage1Q + K * K) / a0;
    c.b1 = 2.0 * (K * K - Vh) / a0;
    c.b2 = (Vh - Vb * K / kBs1770Stage1Q + K * K) / a0;
    c.a1 = 2.0 * (K * K - 1.0) / a0;
    c.a2 = (1.0 - K / kBs1770Stage1Q + K * K) / a0;
    return c;
}

// Stage 2: the RLB high-pass, roughly 38 Hz. b0/b1/b2 are exactly 1/-2/1 at
// every rate -- that is a property of the prototype, not a rounding of the
// published table, so they are written as literals.
inline LoudnessBiquadCoeffs bs1770_stage2_coeffs(uint32_t sample_rate)
{
    const double fs = static_cast<double>(loudness_usable_rate(sample_rate));
    const double K  = std::tan(3.14159265358979323846 * kBs1770Stage2Hz / fs);
    const double d  = 1.0 + K / kBs1770Stage2Q + K * K;

    LoudnessBiquadCoeffs c;
    c.b0 =  1.0;
    c.b1 = -2.0;
    c.b2 =  1.0;
    c.a1 = 2.0 * (K * K - 1.0) / d;
    c.a2 = (1.0 - K / kBs1770Stage2Q + K * K) / d;
    return c;
}
