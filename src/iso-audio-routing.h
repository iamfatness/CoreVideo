#pragma once
#include <cstdint>

inline bool audio_receives_mix(bool isolated, bool audience, bool recording)
{
    return !isolated && !audience && !recording;
}
inline bool audio_receives_participant(uint32_t target, uint32_t speaker, bool isolated, bool recording)
{
    return target != 0 && target == speaker && (isolated || recording);
}
inline bool audio_claims_audience(bool isolated, bool recording)
{
    return isolated && !recording;
}
