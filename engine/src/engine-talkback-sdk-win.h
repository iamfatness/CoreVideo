#pragma once
#include "talkback-sdk.h"
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"

#if defined(WIN32)
#include <windows.h>
#endif

// SDKERR_TOO_FREQUENT_CALL is enum POSITION 18 in the Windows SDKError enum;
// referenced by name, never by the literal, because a header revision that
// inserts a value ahead of it would silently retarget the backoff.
inline TalkbackResult talkback_win_result(ZOOMSDK::SDKError e)
{
    switch (e) {
    case ZOOMSDK::SDKERR_SUCCESS:            return TalkbackResult::Ok;
    case ZOOMSDK::SDKERR_TOO_FREQUENT_CALL:  return TalkbackResult::TooFrequent;
    case ZOOMSDK::SDKERR_NO_PERMISSION:      return TalkbackResult::NoPermission;
    default:                                 return TalkbackResult::Unknown;
    }
}

// The reverse of engine-json.h's zchar_to_utf8(): every SDK call on
// IMeetingTalkbackController takes a channel id as `const zchar_t*`
// (wchar_t* on Windows), but the seam's currency is UTF-8 (src/talkback-sdk.h
// -- picked so the interface itself needs no Zoom type). Channel ids are
// Zoom-generated opaque tokens, never operator-entered text, so this only
// ever needs to round-trip ASCII in practice, but it goes through the real
// Windows conversion API rather than assuming that.
inline std::basic_string<zchar_t> talkback_utf8_to_zchar(const std::string &s)
{
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::basic_string<zchar_t> out(static_cast<size_t>(len - 1), L'\0');
    if (!out.empty())
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

class TalkbackWinSdk : public TalkbackSdk {
public:
    explicit TalkbackWinSdk(ZOOMSDK::IMeetingTalkbackController *ctrl)
        : m_ctrl(ctrl) {}

    bool is_meeting_support_talkback() override
    {
        return m_ctrl && m_ctrl->IsMeetingSupportTalkBack();
    }

    TalkbackResult create_channel(uint32_t count) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        return talkback_win_result(m_ctrl->CreateChannel(count));
    }

    // The Begin/Add/Execute sequence lives HERE and nowhere above. On this
    // platform an invite is three calls that must not interleave with another
    // batch; on macOS it is one call. The ladder is entitled to know neither.
    TalkbackResult invite_users(const std::string &channel_id,
                                const std::vector<uint32_t> &user_ids) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        const std::basic_string<zchar_t> id_z = talkback_utf8_to_zchar(channel_id);
        m_ctrl->BeginBatchInviteUsers(id_z.c_str());
        for (uint32_t id : user_ids)
            m_ctrl->AddUserToInvite(id);
        return talkback_win_result(m_ctrl->ExecuteBatchInviteUsers());
    }

    TalkbackResult destroy_channels(
        const std::vector<std::string> &channel_ids) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        m_ctrl->BeginBatchDestroyChannels();
        // Every id's zchar_t form must outlive AddChannelToDestroy() but need
        // not outlive this whole function -- collected up front only so the
        // loop below is not re-deriving the same conversion issue site by
        // site.
        std::vector<std::basic_string<zchar_t> > ids_z;
        ids_z.reserve(channel_ids.size());
        for (const auto &id : channel_ids) ids_z.push_back(talkback_utf8_to_zchar(id));
        for (const auto &id_z : ids_z)
            m_ctrl->AddChannelToDestroy(id_z.c_str());
        return talkback_win_result(m_ctrl->ExecuteBatchDestroyChannels());
    }

    TalkbackResult send_audio(const std::string &channel_id, const char *data,
                              uint32_t len, uint32_t sample_rate,
                              bool stereo) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        const std::basic_string<zchar_t> id_z = talkback_utf8_to_zchar(channel_id);
        return talkback_win_result(m_ctrl->SendAudioDataToChannel(
            id_z.c_str(), data, len, sample_rate,
            stereo ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                   : ZOOMSDK::ZoomSDKAudioChannel_Mono));
    }

    TalkbackResult set_background_volume(const std::string &channel_id,
                                         float volume) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        const std::basic_string<zchar_t> id_z = talkback_utf8_to_zchar(channel_id);
        return talkback_win_result(
            m_ctrl->SetChannelBackgroundVolume(id_z.c_str(), volume));
    }

    void set_events(TalkbackSdkEvents *events) override { m_events = events; }

private:
    ZOOMSDK::IMeetingTalkbackController *m_ctrl = nullptr;
    TalkbackSdkEvents *m_events = nullptr;
};
