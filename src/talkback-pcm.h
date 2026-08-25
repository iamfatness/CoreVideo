#pragma once
//
// talkback-pcm.h — libobs planar float to Zoom interleaved int16.
//
// libobs delivers audio as PLANAR float (one contiguous buffer per channel,
// `frames` samples each). The Zoom talkback API wants INTERLEAVED 16-bit PCM
// with a byte length that is a multiple of 2. This header is the only place
// that conversion happens.
//
// Pure: no libobs types in the signature (callers pass plain float pointers),
// so it can be pinned by a test with no OBS and no meeting.
//
// THE CLAMP IS NOT DEFENSIVE PADDING. An OBS source with gain above unity
// legitimately produces samples beyond +/-1.0. Casting those straight to
// int16 WRAPS -- a loud passage becomes a full-scale square wave, which is
// the single worst sound to put in a director's ear. NaN maps to silence for
// the same reason: an undefined float cast is an undefined sample value.
//
#include <cmath>
#include <cstddef>
#include <cstdint>

// Bytes needed for `frames` frames of `channels` interleaved int16 samples.
// Always even, so the SDK's "dataLength must be a multiple of 2" holds by
// construction rather than by inspection at the call site.
inline std::size_t talkback_pcm_bytes(std::size_t frames, uint32_t channels)
{
    return frames * static_cast<std::size_t>(channels) * sizeof(int16_t);
}

// One sample, clamped and scaled. Kept separate so the test can reason about
// the scale factor without going through the interleaver.
inline int16_t talkback_pcm_sample(float v)
{
    // NaN fails every comparison, so test for it explicitly rather than
    // relying on the clamps below to catch it.
    if (std::isnan(v)) return 0;
    if (v >=  1.0f) return  32767;
    if (v <= -1.0f) return -32767;   // symmetric with +full scale; -32768 is
                                     // reachable in int16 but asymmetric, and
                                     // symmetry matters more than one LSB here
    const float scaled = v * 32767.0f;
    return static_cast<int16_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

// Interleave `channels` planes of `frames` floats into `out`.
// `planes[c]` must hold at least `frames` samples. Degenerate input is a
// no-op: a tap can fire with a null plane during source teardown, and a
// crash on the audio thread would take OBS with it.
inline void talkback_pcm_interleave(const float *const *planes,
                                    std::size_t frames,
                                    uint32_t channels,
                                    int16_t *out)
{
    if (planes == nullptr || out == nullptr || frames == 0 || channels == 0)
        return;
    for (uint32_t c = 0; c < channels; ++c)
        if (planes[c] == nullptr) return;

    for (std::size_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < channels; ++c)
            out[f * channels + c] = talkback_pcm_sample(planes[c][f]);
}

// Rates IZoomSDKAudioRawDataSender documents as accepted. We pass OBS's rate
// through rather than resampling: a resampler is a whole subsystem, and OBS
// runs at 48kHz by default, which the SDK explicitly recommends. An
// unsupported rate must be reported loudly, never silently resampled or
// silently sent -- a wrong-rate send is heard as a chipmunk or a drawl.
inline bool talkback_pcm_rate_supported(uint32_t sample_rate)
{
    switch (sample_rate) {
    case 8000: case 16000: case 32000: case 44100:
    case 48000: case 50000: case 50400: case 96000: case 192000:
        return true;
    default:
        return false;
    }
}
