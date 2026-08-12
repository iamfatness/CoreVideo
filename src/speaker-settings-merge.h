// src/speaker-settings-merge.h
#pragma once

#include <cstdint>

// What a single source carries in its own obs_data for the speaker director.
// The `has_` flags mirror obs_data_has_user_value(): false means the property
// was never set on this source, not that it was set to zero.
struct SpeakerSourceValues {
    bool     has_sensitivity = false;
    uint32_t sensitivity_ms  = 0;
    bool     has_hold        = false;
    uint32_t hold_ms         = 0;
    bool     has_exclude_1   = false;
    uint32_t exclude_1       = 0;
    bool     has_exclude_2   = false;
    uint32_t exclude_2       = 0;
};

// The global, dock-owned settings the director is configured from.
struct SpeakerGlobalSettings {
    uint32_t sensitivity_ms = 500;
    uint32_t hold_ms        = 2000;
    uint32_t exclude_1      = 0;
    uint32_t exclude_2      = 0;
};

// Each field is carried over only if this source actually has one. An unset
// property is silence, not a zero: the source is not asking for anything, so
// whatever the dock holds stands. Explicitly setting a property to zero IS an
// edit and does apply — that is how an operator turns an exclusion off from the
// source's own properties.
inline SpeakerGlobalSettings merge_speaker_settings(
    SpeakerGlobalSettings current, const SpeakerSourceValues &source)
{
    if (source.has_sensitivity)
        current.sensitivity_ms = source.sensitivity_ms;
    if (source.has_hold)
        current.hold_ms = source.hold_ms;
    if (source.has_exclude_1)
        current.exclude_1 = source.exclude_1;
    if (source.has_exclude_2)
        current.exclude_2 = source.exclude_2;
    return current;
}
