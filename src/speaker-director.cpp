#include "speaker-director.h"

#include <algorithm>

SpeakerDirector &SpeakerDirector::instance()
{
    static SpeakerDirector inst;
    return inst;
}

void SpeakerDirector::configure(uint32_t sensitivity_ms, uint32_t hold_ms,
                                bool require_video)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sensitivity_ms = sensitivity_ms;
    m_hold_ms = hold_ms;
    m_require_video = require_video;
}

void SpeakerDirector::reset()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_roster.clear();
    m_raw_speaker_id = 0;
    m_directed_speaker_id = 0;
    m_candidate_speaker_id = 0;
    m_last_speaker_id = 0;
    m_manual_speaker_id = 0;
    m_candidate_since_ms = 0;
    m_last_switch_ms = 0;
}

bool SpeakerDirector::participant_allowed_locked(uint32_t participant_id) const
{
    if (participant_id == 0)
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

uint32_t SpeakerDirector::choose_candidate_locked(uint32_t raw_speaker_id) const
{
    if (participant_allowed_locked(raw_speaker_id))
        return raw_speaker_id;

    for (const auto &p : m_roster) {
        if (p.is_talking && participant_allowed_locked(p.user_id))
            return p.user_id;
    }
    return 0;
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

    if (!participant_allowed_locked(m_directed_speaker_id))
        m_directed_speaker_id = 0;
    if (m_manual_speaker_id != 0 && !participant_allowed_locked(m_manual_speaker_id))
        m_manual_speaker_id = 0;
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
    if (m_manual_speaker_id != 0) {
        if (participant_allowed_locked(m_manual_speaker_id))
            return promote_locked(m_manual_speaker_id, now_ms);
        m_manual_speaker_id = 0;
    }

    if (!participant_allowed_locked(m_directed_speaker_id))
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
    s.require_video = m_require_video;
    s.manual_active = m_manual_speaker_id != 0;
    return s;
}
