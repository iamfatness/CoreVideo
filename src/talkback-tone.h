#pragma once
//
// talkback-tone.h — the generated tone the talkback probe sends.
//
// A generated sine rather than an audio asset: no file to ship, no format to
// get wrong, and a listener can confirm "steady tone" or "no tone" without
// ambiguity. That matters because the probe's whole job is to answer a yes/no
// question about entitlement, and an ambiguous result answers nothing.
//
// Phase is an ABSOLUTE sample index supplied by the caller, not internal
// state. Buffers are sent one after another; restarting phase at each buffer
// boundary steps the waveform discontinuously, which is audible as a click at
// the buffer rate (~100/sec). A listener would report that as broken audio and
// we would be debugging a transport that was working correctly.
//
// Free of Qt / OBS / Zoom SDK dependencies so it can be pinned by a test with
// no engine and no meeting.
//
#include <cmath>
#include <cstddef>
#include <cstdint>

// Fills `out` with `count` mono 16-bit samples of a sine at `freq_hz`,
// continuing from absolute sample index `start_index`. Returns the next
// absolute index, so the caller chains successive buffers by feeding the
// return value back in.
//
// `amplitude` is 0.0-1.0 of full scale. Kept below 1.0 by callers so the
// int16 conversion cannot wrap on rounding.
inline uint64_t talkback_tone_fill(int16_t *out,
                                   std::size_t count,
                                   uint64_t start_index,
                                   uint32_t sample_rate,
                                   double freq_hz,
                                   double amplitude)
{
    if (out == nullptr || count == 0 || sample_rate == 0)
        return start_index;

    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double step = kTwoPi * freq_hz / static_cast<double>(sample_rate);

    for (std::size_t i = 0; i < count; ++i) {
        // Phase from the absolute index, never from a running accumulator:
        // an accumulator drifts, and more importantly it would have to be
        // stored somewhere, which is what makes chained calls discontinuous.
        const double phase = step * static_cast<double>(start_index + i);
        const double v = std::sin(phase) * amplitude * 32767.0;
        out[i] = static_cast<int16_t>(v < 0.0 ? v - 0.5 : v + 0.5);
    }
    return start_index + count;
}
