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

// The standard's absolute offset. It exists to cancel the K-weighting's
// +0.691 dB gain at 997 Hz, which is why a 1 kHz tone's LUFS value equals
// 10*log10 of its un-weighted mean square exactly.
constexpr double kLoudnessOffsetDb = -0.691;

// Block/hop geometry. 400 ms blocks advancing every 100 ms is 75% overlap,
// which is what BS.1770-4 specifies for gated integration; momentary IS one
// such block, and short-term is 30 hops.
constexpr uint32_t kLoudnessHopMs        = 100;
constexpr uint32_t kLoudnessMomentaryHops = 4;   // 400 ms
constexpr uint32_t kLoudnessShortTermHops = 30;  // 3 s

// L = -0.691 + 10*log10(sum of G_i * z_i). Returns -HUGE_VAL for a
// non-positive mean square rather than letting log10 produce -inf/NaN at an
// arbitrary call site; every caller in this header checks for it.
inline double loudness_lufs_from_mean_square(double z)
{
    if (!(z > 0.0)) return -HUGE_VAL;
    return kLoudnessOffsetDb + 10.0 * std::log10(z);
}

// BS.1770-4 channel weights, in the standard's channel order
// (L, R, C, LFE, Ls, Rs). Zoom participant audio is mono or stereo, so in
// practice only the G = 1.0 terms are ever reached -- but a source configured
// for more channels must not silently weight a surround channel as if it were
// a front one, and the LFE must not be counted at all.
inline double loudness_channel_weight(uint16_t channels, uint16_t channel)
{
    if (channels <= 2) return 1.0;
    switch (channel) {
    case 0: case 1: case 2: return 1.00;  // L, R, C
    case 3:                 return 0.00;  // LFE is excluded, not attenuated
    case 4: case 5:         return 1.41;  // Ls, Rs
    default:                return 0.00;
    }
}

// A running BS.1770-4 measurement for ONE participant.
//
// OWNERSHIP: not thread-safe and deliberately so. In the plugin exactly one
// thread -- the audio lane that owns output_audio_frame() -- feeds it, under
// the same ctx->mtx that already guards the source's timeline, and readers
// take that mutex to copy the three numbers out. Adding a lock in here would
// put one on the media path for no gain.
struct LoudnessMeter {
    uint32_t sample_rate = 0;
    uint16_t channels    = 0;

    LoudnessBiquadCoeffs c1{};
    LoudnessBiquadCoeffs c2{};
    std::vector<LoudnessBiquadState> s1;   // stage 1 state, one per channel
    std::vector<LoudnessBiquadState> s2;   // stage 2 state, one per channel

    // Current partial 100 ms hop.
    uint32_t hop_frames = 0;      // frames per hop at the configured rate
    uint32_t hop_filled = 0;
    double   hop_acc    = 0.0;    // sum over frames of sum_ch(G * y^2)

    // The last kLoudnessShortTermHops completed hops, newest at
    // (hop_total - 1) % kLoudnessShortTermHops.
    double   hop_ring[kLoudnessShortTermHops] = {};
    uint64_t hop_total = 0;

    // The gated integration window -- ONE PANELIST'S MIC CHECK, not the
    // session. Each entry is the mean square of a 400 ms block that cleared
    // the absolute gate. Held as values rather than a running sum because the
    // relative gate has to re-examine every block once the absolute-gated
    // mean is known.
    std::vector<double> gated;
    size_t   gated_head  = 0;   // ring write position once `gated` is full
    uint64_t gated_total = 0;   // blocks ever admitted, never wrapped
};

// (Re)configures for a rate/channel count and clears all filter state. Called
// automatically by loudness_meter_feed_int16() whenever the wire format
// changes -- which it can, mid-source: Zoom renegotiates, and the operator's
// Mix/Isolated role flip changes the channel count on the same subscription.
// Carrying filter history across that would smear one format's transient into
// the other's measurement.
inline void loudness_meter_configure(LoudnessMeter &m, uint32_t sample_rate,
                                     uint16_t channels)
{
    const uint32_t rate = loudness_usable_rate(sample_rate);
    m.sample_rate = rate;
    m.channels    = channels == 0 ? 1 : channels;
    m.c1 = bs1770_stage1_coeffs(rate);
    m.c2 = bs1770_stage2_coeffs(rate);
    m.s1.assign(m.channels, LoudnessBiquadState{});
    m.s2.assign(m.channels, LoudnessBiquadState{});
    m.hop_frames = (rate * kLoudnessHopMs) / 1000;
    if (m.hop_frames == 0) m.hop_frames = 1;
    m.hop_filled = 0;
    m.hop_acc    = 0.0;
    for (uint32_t i = 0; i < kLoudnessShortTermHops; ++i) m.hop_ring[i] = 0.0;
    m.hop_total = 0;
}

// Hook the gated integrator into the hop boundary. Defined in Task 3; the
// forward declaration keeps feed_int16 below unchanged when it lands.
inline void loudness_meter_on_hop_complete(LoudnessMeter &m);

// Feeds interleaved 16-bit signed PCM -- the format the engine writes into
// the SHM ring, unconverted.
//
// SCALING: /32768.0, not /32767.0. int16 is asymmetric and full negative
// scale is -32768; dividing by 32767 would let a legitimate sample exceed
// -1.0 and is the wrong direction for a measurement.
//
// PARTIAL BUFFERS ARE THE NORMAL CASE. Zoom delivers ~10 ms buffers and one
// media event can carry eight of them, so a 100 ms hop is assembled from many
// calls. The hop boundary is decided by frame count alone and never by call
// boundaries, which is what makes "feed the whole drain loop" identical to
// "feed one big buffer" -- pinned as chunk invariance in the test.
inline void loudness_meter_feed_int16(LoudnessMeter &m, const int16_t *pcm,
                                      size_t frames, uint16_t channels,
                                      uint32_t sample_rate)
{
    if (pcm == nullptr || frames == 0 || channels == 0) return;
    if (m.sample_rate != loudness_usable_rate(sample_rate) ||
        m.channels != channels) {
        loudness_meter_configure(m, sample_rate, channels);
    }

    for (size_t f = 0; f < frames; ++f) {
        double frame_sum = 0.0;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            const double g = loudness_channel_weight(channels, ch);
            const double x = static_cast<double>(pcm[f * channels + ch]) /
                             32768.0;
            const double y1 = loudness_biquad_step(m.c1, m.s1[ch], x);
            const double y2 = loudness_biquad_step(m.c2, m.s2[ch], y1);
            // The filters run even for a zero-weight channel: their state is
            // per channel and skipping them would make the LFE's history
            // depend on how long it had been zero-weighted.
            frame_sum += g * y2 * y2;
        }
        m.hop_acc += frame_sum;
        if (++m.hop_filled >= m.hop_frames) {
            const double hop_mean = m.hop_acc /
                                    static_cast<double>(m.hop_frames);
            m.hop_ring[m.hop_total % kLoudnessShortTermHops] = hop_mean;
            ++m.hop_total;
            m.hop_acc    = 0.0;
            m.hop_filled = 0;
            loudness_meter_on_hop_complete(m);
        }
    }
}

// Mean of the newest `n` completed hops. False when fewer than `n` exist --
// which is the honest answer for a panelist who has just been subscribed, and
// is why every getter here returns bool rather than a sentinel loudness.
inline bool loudness_hop_mean(const LoudnessMeter &m, uint32_t n, double *out)
{
    if (n == 0 || n > kLoudnessShortTermHops || m.hop_total < n) return false;
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t idx = m.hop_total - 1 - i;
        sum += m.hop_ring[idx % kLoudnessShortTermHops];
    }
    *out = sum / static_cast<double>(n);
    return true;
}

// Momentary (M): one 400 ms block, ungated.
inline bool loudness_meter_momentary(const LoudnessMeter &m, double *out_lufs)
{
    double z = 0.0;
    if (!loudness_hop_mean(m, kLoudnessMomentaryHops, &z)) return false;
    const double l = loudness_lufs_from_mean_square(z);
    if (!std::isfinite(l)) return false;
    *out_lufs = l;
    return true;
}

// Short-term (S): 3 s, ungated. The number an operator reads while the
// panelist is talking.
inline bool loudness_meter_short_term(const LoudnessMeter &m, double *out_lufs)
{
    double z = 0.0;
    if (!loudness_hop_mean(m, kLoudnessShortTermHops, &z)) return false;
    const double l = loudness_lufs_from_mean_square(z);
    if (!std::isfinite(l)) return false;
    *out_lufs = l;
    return true;
}

// BS.1770-4's two gates. The absolute one discards silence for free, which is
// exactly the mechanism a panel needs: a panelist is silent roughly 80% of a
// panel, and an ungated integrated reading over that measures the meeting
// rather than the microphone (measured: 4 s of -20 LUFS speech inside 20 s
// reads -27.08 ungated). The relative one then discards the quiet tail so the
// answer describes the speech.
constexpr double kLoudnessAbsoluteGateLufs = -70.0;
constexpr double kLoudnessRelativeGateLu   = -10.0;

// 6000 blocks is 10 minutes of continuously-gated audio at a 100 ms hop. A
// mic check is 20-60 s (~200-600 blocks), so this is never reached in the
// use this was built for; past it the window keeps the most RECENT 10 minutes
// rather than growing without bound. Documented rather than silent, because
// "the oldest audio quietly leaves the window" is a real semantic and an
// operator who leaves a board running all show is entitled to know it.
constexpr size_t kLoudnessMaxGatedBlocks = 6000;

// Called at every completed 100 ms hop. A 400 ms block is the newest four
// hops, so admitting one block per hop is the standard's 75% overlap.
inline void loudness_meter_on_hop_complete(LoudnessMeter &m)
{
    double z = 0.0;
    if (!loudness_hop_mean(m, kLoudnessMomentaryHops, &z)) return;
    const double l = loudness_lufs_from_mean_square(z);
    if (!std::isfinite(l) || l <= kLoudnessAbsoluteGateLufs) return;

    if (m.gated.size() < kLoudnessMaxGatedBlocks) {
        m.gated.push_back(z);
    } else {
        m.gated[m.gated_head] = z;
        m.gated_head = (m.gated_head + 1) % kLoudnessMaxGatedBlocks;
    }
    ++m.gated_total;
}

// Starts this source's check window over. Clears the gated blocks and the hop
// history, but NOT the biquad state: the filters describe the signal that is
// still arriving, and zeroing them mid-stream would inject a transient into
// the first block of the new window.
inline void loudness_meter_reset_window(LoudnessMeter &m)
{
    m.gated.clear();
    m.gated_head  = 0;
    m.gated_total = 0;
    m.hop_acc     = 0.0;
    m.hop_filled  = 0;
    for (uint32_t i = 0; i < kLoudnessShortTermHops; ++i) m.hop_ring[i] = 0.0;
    m.hop_total   = 0;
}

// Blocks admitted to the current window. A board uses this to decide whether
// an integrated reading is worth showing: the spec's 20 s mic check yields
// ~200 blocks, so a handful of blocks is a cough, not a check.
inline uint64_t loudness_meter_gated_blocks(const LoudnessMeter &m)
{
    return m.gated_total;
}

// Integrated (I): the two-pass gate, over the current check window.
// False means "this panelist has not produced a measurable check yet", which
// is a different statement from any loudness value and must stay
// distinguishable all the way to the board.
inline bool loudness_meter_integrated(const LoudnessMeter &m, double *out_lufs)
{
    if (m.gated.empty()) return false;

    double sum = 0.0;
    for (double z : m.gated) sum += z;
    const double abs_mean_lufs =
        loudness_lufs_from_mean_square(sum / static_cast<double>(m.gated.size()));
    if (!std::isfinite(abs_mean_lufs)) return false;

    const double relative_threshold = abs_mean_lufs + kLoudnessRelativeGateLu;
    double sum2 = 0.0;
    size_t n2 = 0;
    for (double z : m.gated) {
        // Strictly greater, per BS.1770-4: a block exactly on the threshold
        // is excluded.
        if (loudness_lufs_from_mean_square(z) > relative_threshold) {
            sum2 += z;
            ++n2;
        }
    }
    if (n2 == 0) return false;

    const double l = loudness_lufs_from_mean_square(sum2 /
                                                    static_cast<double>(n2));
    if (!std::isfinite(l)) return false;
    *out_lufs = l;
    return true;
}
