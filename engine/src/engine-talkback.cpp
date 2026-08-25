#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::write -- an inline fn in a namespace,
                             // so it must be INCLUDED, never forward-declared
#include "talkback-tone.h"
#include "engine-json.h"     // zchar_to_utf8 / json_escape / json_str (Step 3a)

#include <chrono>
#include <string>

namespace {
// A hung AwaitingChannel/AwaitingInvite rung and a genuine permission denial
// look identical from the outside (both are "nothing happened yet") unless
// something bounds how long we wait -- see the timeout block in tick().
constexpr std::chrono::milliseconds kAwaitTimeout{10000};

// BeginBatchDestroyChannels/AddChannelToDestroy/ExecuteBatchDestroyChannels
// can themselves fail synchronously (distinct from the channel simply never
// confirming destruction). Give up on the first failure and the "always
// destroy" guarantee this file claims is false; retry a bounded number of
// times instead.
constexpr uint32_t kMaxDestroyAttempts = 5;
} // namespace

void EngineTalkback::report(const std::string &stage, const std::string &fields)
{
    std::string line = R"({"cmd":"talkback_probe","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

void EngineTalkback::probe(ZOOMSDK::IMeetingService *svc,
                           const std::string &participant_name)
{
    // Re-entrancy guard: Task 5 wires this to a control-API command a human
    // can send twice. A second call while a channel is live would otherwise
    // unconditionally reset m_channel_id_z out from under the live ladder,
    // discarding the only handle we have to destroy it -- refusing is
    // correct for a probe; there is nothing sensible to queue or cancel.
    const Phase current = m_phase.load(std::memory_order_acquire);
    if (current != Phase::Idle && current != Phase::Done) {
        report("busy", R"("phase":)" + std::to_string(static_cast<int>(current)));
        return;
    }

    m_svc = svc;
    m_participant_name = participant_name;
    m_phase.store(Phase::Idle, std::memory_order_release);
    m_channel_id.clear();
    m_channel_id_z.clear();
    m_participant_id = 0;
    m_tone_index = 0;
    m_buffers_sent = 0;
    m_destroy_attempts = 0;

    if (!m_svc) {
        report("controller", R"("ok":false,"reason":"no_meeting_service")");
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }

    // RUNG 1: does the controller exist at all on this SDK/account?
    m_ctrl = m_svc->GetMeetingTalkbackController();
    report("controller", std::string(R"("ok":)") + (m_ctrl ? "true" : "false"));
    if (!m_ctrl) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }

    // RUNG 2: the meeting-level gate. This is the one we expect Enhanced Media
    // to satisfy, and the one that decides whether the feature is viable.
    const bool supported = m_ctrl->IsMeetingSupportTalkBack();
    report("meeting_supported",
           std::string(R"("supported":)") + (supported ? "true" : "false"));
    if (!supported) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }

    const ZOOMSDK::SDKError set_err = m_ctrl->SetEvent(this);
    report("set_event", R"("code":)" + std::to_string(static_cast<int>(set_err)));
    if (set_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }

    // RUNG 3: create exactly one channel. Max 16 exist; we make one and
    // destroy it, so a failed probe cannot leak channel budget into the
    // meeting.
    const ZOOMSDK::SDKError create_err = m_ctrl->CreateChannel(1);
    report("create_channel", R"("code":)" +
           std::to_string(static_cast<int>(create_err)));
    if (create_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }
    m_phase_deadline = std::chrono::steady_clock::now() + kAwaitTimeout;
    m_phase.store(Phase::AwaitingChannel, std::memory_order_release);   // continues in onCreateChannelResponse
}

void EngineTalkback::tick()
{
    const Phase phase = m_phase.load(std::memory_order_acquire);

    if (phase == Phase::AwaitingChannel || phase == Phase::AwaitingInvite) {
        // SDKERR_SUCCESS on CreateChannel/ExecuteBatchInviteUsers is only a
        // promise the call was accepted, not that onCreateChannelResponse /
        // onChannelUserJoinResponse will ever fire. Without this deadline a
        // swallowed callback hangs here forever and reports nothing -- a
        // hang IS silence, and it is indistinguishable from a permission
        // failure at exactly the moment we need to tell the two apart.
        if (std::chrono::steady_clock::now() >= m_phase_deadline) {
            report("timeout", R"("phase":)" + std::to_string(static_cast<int>(phase)));
            m_phase.store(Phase::Destroying, std::memory_order_release);
        }
        return;
    }

    if (phase == Phase::Sending) {
        // 10ms of mono 48kHz, matching the buffer size Zoom itself uses on the
        // receive side. dataLength must be a multiple of 2 (it is: 480 * 2).
        constexpr uint32_t kRate    = 48000;
        constexpr std::size_t kCount = 480;
        constexpr uint32_t kBuffers  = 300;   // ~3 seconds

        int16_t pcm[kCount];
        m_tone_index = talkback_tone_fill(pcm, kCount, m_tone_index, kRate, 440.0, 0.5);

        const ZOOMSDK::SDKError e = m_ctrl->SendAudioDataToChannel(
            m_channel_id_z.c_str(), reinterpret_cast<const char *>(pcm),
            static_cast<unsigned int>(kCount * sizeof(int16_t)), kRate,
            ZOOMSDK::ZoomSDKAudioChannel_Mono);

        // Report the first send and any failure, never all 300 -- 300 pipe
        // lines is the message-storm shape this codebase already has a live
        // incident about.
        if (m_buffers_sent == 0 || e != ZOOMSDK::SDKERR_SUCCESS) {
            report("send", R"("buffer":)" + std::to_string(m_buffers_sent) +
                   R"(,"code":)" + std::to_string(static_cast<int>(e)));
        }
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            m_phase.store(Phase::Destroying, std::memory_order_release);
            return;
        }
        if (++m_buffers_sent >= kBuffers) {
            report("sent", R"("buffers":)" + std::to_string(m_buffers_sent));
            m_phase.store(Phase::Destroying, std::memory_order_release);
        }
        return;
    }

    if (phase == Phase::Destroying) {
        // Always destroy, on every exit path: a leaked channel consumes one of
        // the meeting's 16 for as long as the meeting lasts. The synchronous
        // Begin/Add/Execute chain can itself fail (separate from the channel
        // never confirming destruction via onDestroyChannelResponse); tick()
        // is already called repeatedly, so retry a bounded number of times
        // rather than abandoning the channel on the first failure -- giving
        // up immediately was exactly the leak this comment claims to
        // prevent.
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS && !m_channel_id_z.empty())
            e = m_ctrl->AddChannelToDestroy(m_channel_id_z.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->ExecuteBatchDestroyChannels();
        report("destroy", R"("code":)" + std::to_string(static_cast<int>(e)) +
               R"(,"attempt":)" + std::to_string(m_destroy_attempts + 1));

        if (e == ZOOMSDK::SDKERR_SUCCESS) {
            m_phase.store(Phase::Done, std::memory_order_release);
            return;
        }

        if (++m_destroy_attempts >= kMaxDestroyAttempts) {
            // Out of retries: the channel is now stranded for the rest of the
            // meeting (one of 16 gone for good) and nothing will self-heal
            // it. Make that visible rather than silently giving up.
            report("destroy_abandoned",
                   R"("channel":")" + json_escape(m_channel_id) +
                   R"(","attempts":)" + std::to_string(m_destroy_attempts));
            m_phase.store(Phase::Done, std::memory_order_release);
        }
    }
}

unsigned int EngineTalkback::resolve_participant(const std::string &name) const
{
    if (!m_svc) return 0;
    auto *part = m_svc->GetMeetingParticipantsController();
    if (!part) return 0;
    ZOOMSDK::IList<unsigned int> *ids = part->GetParticipantsList();
    if (!ids) return 0;
    for (int i = 0; i < ids->GetCount(); ++i) {
        const unsigned int uid = ids->GetItem(i);
        ZOOMSDK::IUserInfo *u = part->GetUserByUserID(uid);
        if (!u) continue;
        if (zchar_to_utf8(u->GetUserName()) == name) return uid;
    }
    return 0;
}

void EngineTalkback::onCreateChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    const std::string id = zchar_to_utf8(channelID);
    report("create_channel_response",
           R"("channel":")" + json_escape(id) + R"(","error":)" +
           std::to_string(static_cast<int>(error)));

    if (m_phase.load(std::memory_order_acquire) != Phase::AwaitingChannel) {
        // Not the callback we're waiting on -- either a stray/duplicate, or
        // (now that tick() times AwaitingChannel out) a genuinely late
        // response that arrived after we already gave up on this rung and
        // moved on, in which case a real channel exists that our own
        // ladder no longer tracks. Touching m_channel_id_z/m_phase here
        // would corrupt whatever probe (if any) is running now, so leave
        // all of that alone -- but a channel that error==OK just handed us
        // must not simply rot for the rest of the meeting, so best-effort
        // destroy it by the ID the callback gave us directly, and make the
        // situation visible either way.
        if (error == TALKBACK_ERROR_OK) {
            ZOOMSDK::SDKError de = m_ctrl->BeginBatchDestroyChannels();
            if (de == ZOOMSDK::SDKERR_SUCCESS) de = m_ctrl->AddChannelToDestroy(channelID);
            if (de == ZOOMSDK::SDKERR_SUCCESS) de = m_ctrl->ExecuteBatchDestroyChannels();
            report("create_channel_response_stray",
                   R"("channel":")" + json_escape(id) +
                   R"(","destroy_code":)" + std::to_string(static_cast<int>(de)));
        }
        return;
    }
    if (error != TALKBACK_ERROR_OK) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }
    m_channel_id = id;              // UTF-8, reporting only
    m_channel_id_z.assign(channelID); // SDK identifier, verbatim -- see header

    // RUNG 4: invite one participant, resolved from a NAME. A raw id would
    // point at nobody after a rejoin and at the wrong person once ids are
    // recycled -- the defect the Companion module fixed in v0.1.44.
    m_participant_id = resolve_participant(m_participant_name);
    if (m_participant_id == 0) {
        report("invite", R"("error":"no_participant_named","name":")" +
               json_escape(m_participant_name) + "\"");
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }

    ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(m_channel_id_z.c_str());
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_participant_id);
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
    report("invite", R"("user_id":)" + std::to_string(m_participant_id) +
           R"(,"code":)" + std::to_string(static_cast<int>(e)));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }
    m_phase_deadline = std::chrono::steady_clock::now() + kAwaitTimeout;
    m_phase.store(Phase::AwaitingInvite, std::memory_order_release);
}

void EngineTalkback::onDestroyChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    report("destroyed",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","error":)" + std::to_string(static_cast<int>(error)));
}

void EngineTalkback::onChannelUserJoinResponse(const zchar_t *channelID,
                                               unsigned int userID, TalkbackError error)
{
    report("invite_response",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","user_id":)" + std::to_string(userID) +
           R"(,"error":)" + std::to_string(static_cast<int>(error)));
    if (m_phase.load(std::memory_order_acquire) != Phase::AwaitingInvite) return;

    // Phase alone can't tell "our invite landed" from "some other invite
    // response landed while we happened to be in this phase" -- correlate
    // the callback to the exact channel and user we are waiting on.
    if (m_channel_id_z != channelID || userID != m_participant_id) {
        report("invite_response_mismatch",
               R"("expected_channel":")" + json_escape(m_channel_id) +
               R"(","expected_user":)" + std::to_string(m_participant_id));
        return;
    }
    if (error != TALKBACK_ERROR_OK) {
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }

    // Duck the main meeting for the person being spoken to, so the tone is
    // unambiguous rather than competing with meeting audio.
    const ZOOMSDK::SDKError vol =
        m_ctrl->SetChannelBackgroundVolume(m_channel_id_z.c_str(), 0.3f);
    report("background_volume", R"("code":)" +
           std::to_string(static_cast<int>(vol)));

    m_tone_index = 0;
    m_buffers_sent = 0;
    // m_channel_id_z was fully written in onCreateChannelResponse before
    // m_phase ever reached AwaitingInvite; this release-store is what makes
    // that write visible to tick() on whatever thread reads Sending next --
    // see the ordering comment on m_phase in the header.
    m_phase.store(Phase::Sending, std::memory_order_release);   // tick() takes it from here
}

void EngineTalkback::onChannelUserLeaveResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onJoinTalkbackChannel(unsigned int) {}
void EngineTalkback::onLeaveTalkbackChannel(unsigned int) {}
void EngineTalkback::onInviterAudioLevel(unsigned int, unsigned int) {}
