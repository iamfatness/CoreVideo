#include "speaker-director.h"

#include <algorithm>

SpeakerDirector &SpeakerDirector::instance()
{
    static SpeakerDirector inst;
    return inst;
}

void SpeakerDirector::configure(uint32_t sensitivity_ms, uint32_t hold_ms,
                                bool require_video,
                                std::vector<uint32_t> excluded_participant_ids)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sensitivity_ms = sensitivity_ms;
    m_hold_ms = hold_ms;
    m_require_video = require_video;
    excluded_participant_ids.erase(
        std::remove(excluded_participant_ids.begin(),
                    excluded_participant_ids.end(), 0),
        excluded_participant_ids.end());
    std::sort(excluded_participant_ids.begin(), excluded_participant_ids.end());
    excluded_participant_ids.erase(
        std::unique(excluded_participant_ids.begin(),
                    excluded_participant_ids.end()),
        excluded_participant_ids.end());
    m_excluded_participant_ids = std::move(excluded_participant_ids);
}

void SpeakerDirector::reset()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_roster.clear();
    m_raw_speaker_id = 0;
    m_directed_speaker_id = 0;
    m_directed_missing_since_ms = 0;
    m_manual_missing_since_ms = 0;
    m_candidate_speaker_id = 0;
    m_last_speaker_id = 0;
    m_manual_speaker_id = 0;
    m_excluded_participant_ids.clear();
    m_candidate_since_ms = 0;
    m_last_switch_ms = 0;
}

// Strict vetting for NEW candidates only. The incumbent is judged by
// enforce_incumbent_eligibility_locked instead: someone who mutes or turns
// their camera off after speaking must stay on the program output.
bool SpeakerDirector::participant_allowed_locked(uint32_t participant_id) const
{
    if (participant_id == 0)
        return false;
    if (participant_excluded_locked(participant_id))
        return false;

    const auto it = std::find_if(m_roster.begin(), m_roster.end(),
        [participant_id](const ParticipantInfo &p) {
            return p.user_id == participant_id;
        });
    if (it == m_roster.end())
        return false;
    if (it->is_muted)
        return false;
    if (m_require_video && !it->has_video)
        return false;
    return true;
}

bool SpeakerDirector::participant_excluded_locked(uint32_t participant_id) const
{
    return std::find(m_excluded_participant_ids.begin(),
                     m_excluded_participant_ids.end(),
                     participant_id) != m_excluded_participant_ids.end();
}

bool SpeakerDirector::participant_in_roster_locked(uint32_t participant_id) const
{
    return std::any_of(m_roster.begin(), m_roster.end(),
        [participant_id](const ParticipantInfo &p) {
            return p.user_id == participant_id;
        });
}

void SpeakerDirector::enforce_incumbent_eligibility_locked(uint64_t now_ms)
{
    constexpr uint64_t kAbsenceGraceMs = 60000;
    const auto enforce = [&](uint32_t &holder, uint64_t &missing_since) {
        if (holder == 0) {
            missing_since = 0;
            return;
        }
        if (participant_excluded_locked(holder)) {
            holder = 0;
            missing_since = 0;
            return;
        }
        if (participant_in_roster_locked(holder)) {
            missing_since = 0;
            return;
        }
        // Absent from the roster: could be a transient blip (engine
        // reconnect) — dethrone only after the grace window.
        if (missing_since == 0)
            missing_since = now_ms;
        else if (now_ms >= missing_since &&
                 now_ms - missing_since > kAbsenceGraceMs) {
            holder = 0;
            missing_since = 0;
        }
    };
    enforce(m_directed_speaker_id, m_directed_missing_since_ms);
    enforce(m_manual_speaker_id, m_manual_missing_since_ms);
}

uint32_t SpeakerDirector::choose_candidate_locked(uint32_t raw_speaker_id) const
{
    if (participant_allowed_locked(raw_speaker_id))
        return raw_speaker_id;

    const auto it = std::find_if(m_roster.begin(), m_roster.end(),
        [this](const ParticipantInfo &p) {
            return p.is_talking && participant_allowed_locked(p.user_id);
        });
    return it != m_roster.end() ? it->user_id : 0;
}

bool SpeakerDirector::promote_locked(uint32_t participant_id, uint64_t now_ms)
{
    if (participant_id == 0 || participant_id == m_directed_speaker_id)
        return false;
    m_last_speaker_id = m_directed_speaker_id;
    m_directed_speaker_id = participant_id;
    m_candidate_speaker_id = 0;
    m_candidate_since_ms = 0;
    m_last_switch_ms = now_ms;
    return true;
}

bool SpeakerDirector::set_manual_speaker(uint32_t participant_id, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!participant_allowed_locked(participant_id))
        return false;
    m_manual_speaker_id = participant_id;
    m_candidate_speaker_id = 0;
    m_candidate_since_ms = 0;
    return promote_locked(participant_id, now_ms);
}

bool SpeakerDirector::clear_manual_speaker(uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_manual_speaker_id == 0)
        return false;
    m_manual_speaker_id = 0;
    m_candidate_speaker_id = 0;
    m_candidate_since_ms = 0;
    return tick_locked(now_ms);
}

bool SpeakerDirector::update_roster(const std::vector<ParticipantInfo> &roster,
                                    uint32_t raw_speaker_id,
                                    uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_roster = roster;
    m_raw_speaker_id = raw_speaker_id;

    enforce_incumbent_eligibility_locked(now_ms);
    if (m_manual_speaker_id != 0)
        return promote_locked(m_manual_speaker_id, now_ms);

    const uint32_t candidate = choose_candidate_locked(raw_speaker_id);
    if (m_directed_speaker_id == 0)
        return promote_locked(candidate, now_ms);

    if (candidate == 0 || candidate == m_directed_speaker_id) {
        m_candidate_speaker_id = 0;
        m_candidate_since_ms = 0;
        return false;
    }

    if (candidate != m_candidate_speaker_id) {
        m_candidate_speaker_id = candidate;
        m_candidate_since_ms = now_ms;
    }

    return tick_locked(now_ms);
}

bool SpeakerDirector::tick_locked(uint64_t now_ms)
{
    enforce_incumbent_eligibility_locked(now_ms);
    if (m_manual_speaker_id != 0)
        return promote_locked(m_manual_speaker_id, now_ms);

    if (m_directed_speaker_id == 0)
        return promote_locked(choose_candidate_locked(m_raw_speaker_id), now_ms);

    if (!participant_allowed_locked(m_candidate_speaker_id))
        return false;

    const uint64_t candidate_age = now_ms - m_candidate_since_ms;
    const uint64_t held_for = now_ms - m_last_switch_ms;
    if (candidate_age < m_sensitivity_ms || held_for < m_hold_ms)
        return false;

    return promote_locked(m_candidate_speaker_id, now_ms);
}

bool SpeakerDirector::tick(uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return tick_locked(now_ms);
}

uint32_t SpeakerDirector::directed_speaker_id() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_directed_speaker_id;
}

SpeakerDirectorSnapshot SpeakerDirector::snapshot(uint64_t now_ms) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    SpeakerDirectorSnapshot s;
    s.raw_speaker_id = m_raw_speaker_id;
    s.directed_speaker_id = m_directed_speaker_id;
    s.candidate_speaker_id = m_candidate_speaker_id;
    s.last_speaker_id = m_last_speaker_id;
    s.manual_speaker_id = m_manual_speaker_id;
    if (m_candidate_speaker_id != 0 && now_ms >= m_candidate_since_ms)
        s.candidate_elapsed_ms = now_ms - m_candidate_since_ms;
    const uint64_t held_for = now_ms >= m_last_switch_ms
        ? now_ms - m_last_switch_ms
        : 0;
    s.hold_remaining_ms = held_for >= m_hold_ms ? 0 : m_hold_ms - held_for;
    s.sensitivity_ms = m_sensitivity_ms;
    s.hold_ms = m_hold_ms;
    s.excluded_participant_ids = m_excluded_participant_ids;
    s.require_video = m_require_video;
    s.manual_active = m_manual_speaker_id != 0;
    return s;
}
