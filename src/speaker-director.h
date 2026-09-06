#pragma once

#include "zoom-types.h"
#include <cstdint>
#include <mutex>
#include <vector>

enum class SpeakerPromotionReason : uint8_t {
    None,
    Automatic,
    ManualTake,
    ForcedVacancy,
};

struct SpeakerPromotionAttribution {
    SpeakerPromotionReason reason = SpeakerPromotionReason::None;
    uint64_t session_id = 0;
    uint64_t sequence = 0;
    uint32_t previous_speaker_id = 0;
    uint32_t promoted_speaker_id = 0;
    uint64_t promoted_at_ms = 0;
    uint32_t effective_sensitivity_ms = 0;
    uint32_t effective_hold_ms = 0;
    uint64_t candidate_age_ms = 0;
    uint64_t incumbent_held_ms = 0;
};

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
    std::vector<uint32_t> excluded_participant_ids;
    bool require_video = true;
    bool manual_active = false;
    SpeakerPromotionAttribution last_promotion;
    std::vector<SpeakerPromotionAttribution> recent_promotions;
};

class SpeakerDirector {
public:
    static SpeakerDirector &instance();

    void configure(uint32_t sensitivity_ms, uint32_t hold_ms,
                   bool require_video = true,
                   std::vector<uint32_t> excluded_participant_ids = {});
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

    bool promote_locked(uint32_t participant_id, uint64_t now_ms,
                        SpeakerPromotionReason reason,
                        uint32_t effective_sensitivity_ms,
                        uint32_t effective_hold_ms,
                        uint64_t candidate_age_ms,
                        uint64_t incumbent_held_ms);
    uint64_t normalize_time_locked(uint64_t now_ms);
    bool participant_allowed_locked(uint32_t participant_id) const;
    bool participant_excluded_locked(uint32_t participant_id) const;
    bool participant_in_roster_locked(uint32_t participant_id) const;
    // Dethrones the incumbent/manual speaker only on exclusion or on
    // absence from the roster beyond the grace window; mute, video-off,
    // and momentary roster blips never dethrone (the Active Speaker output
    // must keep showing the last speaker until someone else takes over).
    void enforce_incumbent_eligibility_locked(uint64_t now_ms);
    uint32_t choose_candidate_locked(uint32_t raw_speaker_id) const;
    // Fills an empty chair. Immediate on a cold start and on the first forced
    // vacancy in a hold window; debounced by sensitivity on repeats.
    bool fill_vacancy_locked(uint32_t candidate, uint64_t now_ms);
    bool tick_locked(uint64_t now_ms);

    mutable std::mutex m_mtx;
    std::vector<ParticipantInfo> m_roster;
    uint32_t m_raw_speaker_id = 0;
    uint32_t m_directed_speaker_id = 0;
    uint64_t m_directed_missing_since_ms = 0;
    uint64_t m_manual_missing_since_ms = 0;
    uint32_t m_candidate_speaker_id = 0;
    uint32_t m_last_speaker_id = 0;
    uint32_t m_manual_speaker_id = 0;
    std::vector<uint32_t> m_excluded_participant_ids;
    uint64_t m_candidate_since_ms = 0;
    uint64_t m_last_switch_ms = 0;
    // When the chair was emptied by enforce_incumbent_eligibility_locked
    // rather than by a cold start, and when the last such emergency fill
    // happened. Together these cap undebounced replacements at one per hold
    // window — see fill_vacancy_locked() and the 2026-08-10 incident.
    uint64_t m_last_forced_fill_ms = 0;
    bool m_forced_vacancy = false;
    uint32_t m_sensitivity_ms = 500;
    uint32_t m_hold_ms = 2000;
    bool m_require_video = true;
    uint64_t m_latest_time_ms = 0;
    uint64_t m_session_id = 0;
    uint64_t m_promotion_sequence = 0;
    SpeakerPromotionAttribution m_last_promotion;
    std::vector<SpeakerPromotionAttribution> m_recent_promotions;
};
