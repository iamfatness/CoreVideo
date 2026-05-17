#pragma once

#include "zoom-types.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct SpeakerDirectorSnapshot {
    uint32_t raw_speaker_id = 0;
    uint32_t directed_speaker_id = 0;
    uint32_t candidate_speaker_id = 0;
    uint32_t last_speaker_id = 0;
    uint32_t manual_speaker_id = 0;
    uint64_t candidate_elapsed_ms = 0;
    uint64_t hold_remaining_ms = 0;
    uint32_t sensitivity_ms = 500;
    uint32_t hold_ms = 2000;
    bool require_video = true;
    bool manual_active = false;
};

class SpeakerDirector {
public:
    static SpeakerDirector &instance();

    void configure(uint32_t sensitivity_ms, uint32_t hold_ms,
                   bool require_video = true);
    bool update_roster(const std::vector<ParticipantInfo> &roster,
                       uint32_t raw_speaker_id,
                       uint64_t now_ms);
    bool tick(uint64_t now_ms);
    void reset();
    bool set_manual_speaker(uint32_t participant_id, uint64_t now_ms);
    bool clear_manual_speaker(uint64_t now_ms);

    uint32_t directed_speaker_id() const;
    SpeakerDirectorSnapshot snapshot(uint64_t now_ms) const;

private:
    SpeakerDirector() = default;

    bool promote_locked(uint32_t participant_id, uint64_t now_ms);
    bool participant_allowed_locked(uint32_t participant_id) const;
    uint32_t choose_candidate_locked(uint32_t raw_speaker_id) const;
    bool tick_locked(uint64_t now_ms);

    mutable std::mutex m_mtx;
    std::vector<ParticipantInfo> m_roster;
    uint32_t m_raw_speaker_id = 0;
    uint32_t m_directed_speaker_id = 0;
    uint32_t m_candidate_speaker_id = 0;
    uint32_t m_last_speaker_id = 0;
    uint32_t m_manual_speaker_id = 0;
    uint64_t m_candidate_since_ms = 0;
    uint64_t m_last_switch_ms = 0;
    uint32_t m_sensitivity_ms = 500;
    uint32_t m_hold_ms = 2000;
    bool m_require_video = true;
};
