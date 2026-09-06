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
    m_last_forced_fill_ms = 0;
    m_forced_vacancy = false;
    m_latest_time_ms = 0;
    ++m_session_id;
    if (m_session_id == 0)
        ++m_session_id;
    m_promotion_sequence = 0;
    m_last_promotion = {};
    m_recent_promotions.clear();
}

uint64_t SpeakerDirector::normalize_time_locked(uint64_t now_ms)
{
    if (now_ms < m_latest_time_ms)
        return m_latest_time_ms;
    m_latest_time_ms = now_ms;
    return now_ms;
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
            return false;
        }
        if (participant_excluded_locked(holder)) {
            holder = 0;
            missing_since = 0;
            return true;
        }
        if (participant_in_roster_locked(holder)) {
            missing_since = 0;
            return false;
        }
        // Absent from the roster: could be a transient blip (engine
        // reconnect) — dethrone only after the grace window.
        if (missing_since == 0)
            missing_since = now_ms;
        else if (now_ms >= missing_since &&
                 now_ms - missing_since > kAbsenceGraceMs) {
            holder = 0;
            missing_since = 0;
            return true;
        }
        return false;
    };
    // Remember that the chair was emptied by enforcement, not by a cold
    // start: fill_vacancy_locked() debounces the replacement in that case.
    // Sticky until something is actually promoted — the vacancy is still a
    // forced one on the ticks that follow.
    if (enforce(m_directed_speaker_id, m_directed_missing_since_ms))
        m_forced_vacancy = true;
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

bool SpeakerDirector::promote_locked(uint32_t participant_id, uint64_t now_ms,
                                     SpeakerPromotionReason reason,
                                     uint32_t effective_sensitivity_ms,
                                     uint32_t effective_hold_ms,
                                     uint64_t candidate_age_ms,
                                     uint64_t incumbent_held_ms)
{
    if (participant_id == 0 || participant_id == m_directed_speaker_id)
        return false;
    const uint32_t previous_speaker_id = m_directed_speaker_id;
    m_last_speaker_id = previous_speaker_id;
    m_directed_speaker_id = participant_id;
    m_candidate_speaker_id = 0;
    m_candidate_since_ms = 0;
    m_last_switch_ms = now_ms;
    m_forced_vacancy = false;
    m_last_promotion.reason = reason;
    m_last_promotion.session_id = m_session_id;
    m_last_promotion.sequence = ++m_promotion_sequence;
    m_last_promotion.previous_speaker_id = previous_speaker_id;
    m_last_promotion.promoted_speaker_id = participant_id;
    m_last_promotion.promoted_at_ms = now_ms;
    m_last_promotion.effective_sensitivity_ms = effective_sensitivity_ms;
    m_last_promotion.effective_hold_ms = effective_hold_ms;
    m_last_promotion.candidate_age_ms = candidate_age_ms;
    m_last_promotion.incumbent_held_ms = incumbent_held_ms;
    constexpr size_t kPromotionHistoryLimit = 16;
    if (m_recent_promotions.size() == kPromotionHistoryLimit)
        m_recent_promotions.erase(m_recent_promotions.begin());
    m_recent_promotions.push_back(m_last_promotion);
    return true;
}

// The chair is empty. This used to be the one path that promoted with NEITHER
// hold NOR sensitivity applied, and on 2026-08-10 that produced active-speaker
// cuts 0.3 s apart on a 2 s hold: a settings-precedence bug flapped the
// exclusion list, enforce_incumbent_eligibility_locked() dethroned the
// directed speaker on nearly every cycle, and each dethrone handed out another
// free cut to whatever choose_candidate_locked() returned at that instant.
//
// Leaving an ineligible incumbent must still be instant — when an operator
// excludes whoever is on air, they expect them off air now — so the fill stays
// immediate for the cases that matter and is only rate-limited on repeats:
//
//   * cold start (reset(), first roster of a meeting): nothing is on air, so
//     there is no cut to debounce. Take the first eligible speaker at once.
//   * first forced vacancy in a hold window: immediate, as before. One
//     operator action, one instant cut.
//   * a second forced vacancy inside that same hold window: this is the flap
//     signature, and the replacement is unvetted — it could be a cough or a
//     half-second of cross-talk. Make it earn the shot through the normal
//     sensitivity window first.
//
// Waiting costs no picture: while the director reports 0, an ActiveSpeaker
// source keeps its current subscription (zoom-source.cpp), so the last speaker
// stays on the program output instead of cutting to black.
//
// Deliberately NOT done here: falling back to m_last_speaker_id. Under the
// exact failure mode being hardened — a flapping eligibility set — the last
// speaker is the participant most likely to have just flapped back into
// eligibility, so preferring them would trade the free-cut bug for a free
// A→B→A ping-pong. Vetting has to be temporal (has this candidate held the
// floor for sensitivity_ms?), not historical.
bool SpeakerDirector::fill_vacancy_locked(uint32_t candidate, uint64_t now_ms)
{
    // Track the candidate even while the chair is empty, so the sensitivity
    // clock runs and a candidate that changes mid-window re-arms it.
    if (candidate != m_candidate_speaker_id) {
        m_candidate_speaker_id = candidate;
        m_candidate_since_ms = candidate != 0 ? now_ms : 0;
    }
    if (candidate == 0)
        return false;

    const bool forced_vacancy = m_forced_vacancy;
    uint32_t effective_sensitivity_ms = 0;
    if (forced_vacancy) {
        const bool immediate_fill_available =
            m_last_forced_fill_ms == 0 || now_ms < m_last_forced_fill_ms ||
            now_ms - m_last_forced_fill_ms >= m_hold_ms;
        if (!immediate_fill_available &&
            (now_ms < m_candidate_since_ms ||
             now_ms - m_candidate_since_ms < m_sensitivity_ms)) {
            return false;
        }
        if (!immediate_fill_available)
            effective_sensitivity_ms = m_sensitivity_ms;
        m_last_forced_fill_ms = now_ms;
    }

    const uint64_t candidate_age = now_ms >= m_candidate_since_ms
        ? now_ms - m_candidate_since_ms : 0;
    const uint64_t incumbent_held_ms = forced_vacancy &&
                                       now_ms >= m_last_switch_ms
        ? now_ms - m_last_switch_ms : 0;
    return promote_locked(candidate, now_ms,
                          forced_vacancy
                              ? SpeakerPromotionReason::ForcedVacancy
                              : SpeakerPromotionReason::Automatic,
                          effective_sensitivity_ms, 0, candidate_age,
                          incumbent_held_ms);
}

bool SpeakerDirector::set_manual_speaker(uint32_t participant_id, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    now_ms = normalize_time_locked(now_ms);
    if (!participant_allowed_locked(participant_id))
        return false;
    m_manual_speaker_id = participant_id;
    m_candidate_speaker_id = 0;
    m_candidate_since_ms = 0;
    const uint64_t held_for = now_ms >= m_last_switch_ms
        ? now_ms - m_last_switch_ms : 0;
    return promote_locked(participant_id, now_ms,
                          SpeakerPromotionReason::ManualTake,
                          0, 0, 0, held_for);
}

bool SpeakerDirector::clear_manual_speaker(uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    now_ms = normalize_time_locked(now_ms);
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
    now_ms = normalize_time_locked(now_ms);
    m_roster = roster;
    m_raw_speaker_id = raw_speaker_id;

    enforce_incumbent_eligibility_locked(now_ms);
    if (m_manual_speaker_id != 0)
        return promote_locked(m_manual_speaker_id, now_ms,
                              SpeakerPromotionReason::ManualTake,
                              0, 0, 0,
                              now_ms >= m_last_switch_ms
                                  ? now_ms - m_last_switch_ms : 0);

    const uint32_t candidate = choose_candidate_locked(raw_speaker_id);
    if (m_directed_speaker_id == 0)
        return fill_vacancy_locked(candidate, now_ms);

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
        return promote_locked(m_manual_speaker_id, now_ms,
                              SpeakerPromotionReason::ManualTake,
                              0, 0, 0,
                              now_ms >= m_last_switch_ms
                                  ? now_ms - m_last_switch_ms : 0);

    if (m_directed_speaker_id == 0) {
        return fill_vacancy_locked(choose_candidate_locked(m_raw_speaker_id),
                                   now_ms);
    }

    if (!participant_allowed_locked(m_candidate_speaker_id))
        return false;

    const uint64_t candidate_age = now_ms - m_candidate_since_ms;
    const uint64_t held_for = now_ms - m_last_switch_ms;
    if (candidate_age < m_sensitivity_ms || held_for < m_hold_ms)
        return false;

    return promote_locked(m_candidate_speaker_id, now_ms,
                          SpeakerPromotionReason::Automatic,
                          m_sensitivity_ms, m_hold_ms,
                          candidate_age, held_for);
}

bool SpeakerDirector::tick(uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    now_ms = normalize_time_locked(now_ms);
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
    now_ms = std::max(now_ms, m_latest_time_ms);
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
    s.last_promotion = m_last_promotion;
    s.recent_promotions = m_recent_promotions;
    return s;
}
