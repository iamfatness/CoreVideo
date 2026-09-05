#pragma once
#include "talkback-host.h"
#include "engine-talkback-sdk-win.h"   // talkback_win_result() -- reused, see below
#include "engine-json.h"               // zchar_to_utf8()
#include "meeting_service_interface.h"
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"

// Windows/Linux adapter for TalkbackHost (macOS-port Task 2b, 2026-09-05).
// Wraps ZOOMSDK::IMeetingService* itself (not the talkback controller --
// that is TalkbackWinSdk's job) because every method here needs a DIFFERENT
// controller off the same meeting service: GetMeetingParticipantsController()
// for the roster and self-lookup, GetMeetingAudioController() for the mute
// calls. Moves the roster walk (engine-talkback.cpp's old
// resolve_participant()/current_roster(), the IList<unsigned int>/IUserInfo
// walk) and the self-mute logic (the old ensure_mic_open()/
// restore_mic_state(), including the null-guard
// `parts ? parts->GetMySelfUser() : nullptr`) into this file, VERBATIM in
// behaviour -- see task-2b-brief.md's Step 2.
class TalkbackWinHost : public TalkbackHost {
public:
    explicit TalkbackWinHost(ZOOMSDK::IMeetingService *svc) : m_svc(svc) {}

    std::vector<TalkbackParticipant> roster() override
    {
        std::vector<TalkbackParticipant> out;
        if (!m_svc) return out;
        auto *part = m_svc->GetMeetingParticipantsController();
        if (!part) return out;
        ZOOMSDK::IList<unsigned int> *ids = part->GetParticipantsList();
        if (!ids) return out;
        out.reserve(static_cast<std::size_t>(ids->GetCount()));
        for (int i = 0; i < ids->GetCount(); ++i) {
            const unsigned int uid = ids->GetItem(i);
            ZOOMSDK::IUserInfo *u = part->GetUserByUserID(uid);
            if (!u) continue;
            TalkbackParticipant p;
            p.user_id = uid;
            p.display_name = zchar_to_utf8(u->GetUserName());
            // Divergence from the brief (see talkback-host.h's own comment
            // on this field): resolve_participant()'s
            // "participant_talkback_support" report line needs this per-user
            // gate, distinct from the meeting-level IsMeetingSupportTalkBack()
            // TalkbackSdk::is_meeting_support_talkback() already answers.
            p.supports_talkback = u->IsSupportTalkback();
            out.push_back(std::move(p));
        }
        return out;
    }

    bool myself(TalkbackParticipant &out) override
    {
        if (!m_svc) return false;
        auto *parts = m_svc->GetMeetingParticipantsController();
        if (!parts) return false;
        ZOOMSDK::IUserInfo *self = parts->GetMySelfUser();
        if (!self) return false;
        out.user_id = self->GetUserID();
        out.display_name = zchar_to_utf8(self->GetUserName());
        out.supports_talkback = self->IsSupportTalkback();
        return true;
    }

    // HAZARD: an unknown mic state must never read as "not muted" -- see
    // talkback-host.h's own comment on this method. Fails CLOSED (returns
    // true, i.e. "muted") whenever the participants controller or the self
    // user cannot be resolved, mirroring the fail-closed discipline this
    // whole feature is built on elsewhere (probe()'s "no_controller"
    // refusals, TalkbackSdk's own no_controller() convention).
    bool is_self_muted() override
    {
        if (!m_svc) return true;
        auto *parts = m_svc->GetMeetingParticipantsController();
        ZOOMSDK::IUserInfo *self = parts ? parts->GetMySelfUser() : nullptr;
        if (!self) return true;
        // THE AUTHORITATIVE READ. IMeetingAudioController has no "am I
        // muted" anywhere in meeting_audio_interface.h -- it has
        // MuteAudio/UnMuteAudio and CanUnMuteBySelf and nothing that reports
        // state. The state lives on IUserInfo::IsAudioMuted() for
        // GetMySelfUser(), the participants controller's own answer and the
        // same one ZComms settled on (see engine-talkback.h's Law 1 comment).
        return self->IsAudioMuted();
    }

    TalkbackResult set_self_muted(bool muted) override
    {
        if (!m_svc) return no_controller();
        auto *parts = m_svc->GetMeetingParticipantsController();
        ZOOMSDK::IUserInfo *self = parts ? parts->GetMySelfUser() : nullptr;
        auto *audio = m_svc->GetMeetingAudioController();
        if (!self || !audio) return no_controller();
        const ZOOMSDK::SDKError e = muted ? audio->MuteAudio(self->GetUserID(), true)
                                           : audio->UnMuteAudio(self->GetUserID());
        m_last_raw_code = static_cast<int>(e);
        // Reuses TalkbackWinSdk's own SDKError->TalkbackResult mapping
        // (talkback_win_result(), engine-talkback-sdk-win.h) rather than a
        // second copy of the same switch -- MuteAudio/UnMuteAudio return the
        // same ZOOMSDK::SDKError enum every other operation on this seam
        // does.
        return talkback_win_result(e);
    }

    int last_raw_code() const override { return m_last_raw_code; }

private:
    // Same "no genuine SDK answer exists for a call that was never made"
    // sentinel TalkbackWinSdk::no_controller() uses -- see talkback-host.h's
    // comment on last_raw_code() for why callers key on TalkbackResult::NotExist
    // specifically to recognise this case.
    TalkbackResult no_controller()
    {
        m_last_raw_code = static_cast<int>(ZOOMSDK::SDKERR_UNKNOWN);
        return TalkbackResult::NotExist;
    }

    ZOOMSDK::IMeetingService *m_svc = nullptr;
    int m_last_raw_code = static_cast<int>(ZOOMSDK::SDKERR_UNKNOWN);
};
