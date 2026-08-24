#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::write -- an inline fn in a namespace,
                             // so it must be INCLUDED, never forward-declared
#include "talkback-tone.h"
#include "engine-json.h"     // zchar_to_utf8 / json_escape / json_str (Step 3a)

#include <string>

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
    m_svc = svc;
    m_participant_name = participant_name;
    m_phase = Phase::Idle;
    m_channel_id.clear();
    m_participant_id = 0;
    m_tone_index = 0;
    m_buffers_sent = 0;

    if (!m_svc) {
        report("controller", R"("ok":false,"reason":"no_meeting_service")");
        m_phase = Phase::Done;
        return;
    }

    // RUNG 1: does the controller exist at all on this SDK/account?
    m_ctrl = m_svc->GetMeetingTalkbackController();
    report("controller", std::string(R"("ok":)") + (m_ctrl ? "true" : "false"));
    if (!m_ctrl) {
        m_phase = Phase::Done;
        return;
    }

    // RUNG 2: the meeting-level gate. This is the one we expect Enhanced Media
    // to satisfy, and the one that decides whether the feature is viable.
    const bool supported = m_ctrl->IsMeetingSupportTalkBack();
    report("meeting_supported",
           std::string(R"("supported":)") + (supported ? "true" : "false"));
    if (!supported) {
        m_phase = Phase::Done;
        return;
    }

    const ZOOMSDK::SDKError set_err = m_ctrl->SetEvent(this);
    report("set_event", R"("code":)" + std::to_string(static_cast<int>(set_err)));
    if (set_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase = Phase::Done;
        return;
    }

    // Rungs 3-6 land in Task 4.
    report("done", R"("reached":"gate_passed")");
    m_phase = Phase::Done;
}

void EngineTalkback::tick() {}

unsigned int EngineTalkback::resolve_participant(const std::string &) const { return 0; }

void EngineTalkback::onCreateChannelResponse(const zchar_t *, TalkbackError) {}
void EngineTalkback::onDestroyChannelResponse(const zchar_t *, TalkbackError) {}
void EngineTalkback::onChannelUserJoinResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onChannelUserLeaveResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onJoinTalkbackChannel(unsigned int) {}
void EngineTalkback::onLeaveTalkbackChannel(unsigned int) {}
void EngineTalkback::onInviterAudioLevel(unsigned int, unsigned int) {}
