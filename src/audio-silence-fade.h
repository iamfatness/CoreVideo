#pragma once

// Ramps in the first buffer published after true digital silence, instead of
// letting it jump straight to full amplitude.
//
// Extracted so it can be tested without OBS or Qt, the same treatment
// audio-timeline.h and director-handover.h get, and for the same reason: the
// only symptom of a regression is bad audio on air.
//
// THE DEFECT THIS EXISTS FOR (2026-08-18, live soak, reported as "clicks and
// pops tied to the active speaker feed"). Instrumented straight from the
// engine's audio SHM ring (bypassing OBS entirely) during a live bot-driven
// meeting: the ring genuinely carries multi-hundred-millisecond runs of
// exact-zero PCM slots -- not missing slots, not torn reads, real ring
// entries whose payload is all zero -- between the participants' spoken
// turns, then resumes at full speech amplitude with no ramp. audio-timeline.h
// already accounts for true GAPS (no callback at all, e.g. a mute); this is
// the different case where Zoom keeps calling back on schedule but the
// content itself is silence, most likely a bot rig whose virtual microphone
// streams continuously and only goes digitally silent between synthesized
// utterances rather than stopping. Zoom's own client conceals this
// perceptually (room tone, its own transitions); the raw SDK callback does
// not, so the instant transition from true zero to full-amplitude speech
// reads as an audible pop. This cannot restore missing speech -- if the
// far end really was silent, that duration stays silent -- it only removes
// the discontinuity at the edge.

#include <cstddef>
#include <cstdint>

// Ramp length on resume from silence. Short enough to be well under the
// ~1 ms rise time that reads as a click, long enough to not itself sound
// like a volume swell; callers multiply by sample_rate/1000 for a frame
// count. Not tied to kZoomBufferMs (10 ms) — the ramp only needs to beat
// the ear's click threshold, not match the SDK's buffer cadence.
constexpr uint32_t kAudioResumeFadeMs = 3;

// True if every sample in the buffer is exactly zero. Cheap early-outs on
// the first nonzero sample; a normal speech buffer trips it on sample 0 or 1.
inline bool audio_buffer_is_silent(const int16_t *pcm, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; ++i) {
        if (pcm[i] != 0) return false;
    }
    return true;
}

// Linearly ramps amplitude from 0 to 1 across the first `ramp_frames` frames
// (all channels scaled identically per frame, so stereo/mono both stay in
// phase). `ramp_frames >= frames` fades the whole buffer, which is the
// correct degenerate case for a buffer shorter than the requested ramp.
inline void audio_apply_resume_fade(int16_t *pcm, uint32_t frames,
                                    uint16_t channels, uint32_t ramp_frames)
{
    if (frames == 0 || channels == 0 || ramp_frames == 0) return;
    const uint32_t n = frames < ramp_frames ? frames : ramp_frames;
    for (uint32_t f = 0; f < n; ++f) {
        // (f+1)/n rather than f/n so the very first frame is not hard-zeroed
        // (that would just move the discontinuity, not remove it) and the
        // last ramped frame reaches unity.
        const float gain = static_cast<float>(f + 1) / static_cast<float>(n);
        for (uint16_t c = 0; c < channels; ++c) {
            const size_t idx = static_cast<size_t>(f) * channels + c;
            pcm[idx] = static_cast<int16_t>(
                static_cast<float>(pcm[idx]) * gain);
        }
    }
}
