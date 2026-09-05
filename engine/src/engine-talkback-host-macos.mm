#import "engine-talkback-host-macos.h"
#import <ZoomSDK/ZoomSDK.h>

// Divergence from the brief worth stating plainly (same discipline as
// engine-talkback-sdk-macos.mm's own divergence note): the brief's Windows
// sketch (TalkbackWinHost) reaches GetMeetingParticipantsController() and
// GetMeetingAudioController() off ZOOMSDK::IMeetingService. The macOS SDK has
// no equivalent split -- there is one ZoomSDKMeetingActionController
// (getMeetingActionController on ZoomSDKMeetingService) that owns BOTH the
// roster/self lookup (getParticipantsList/getUserByUserID/getMyself) AND the
// mute action (actionMeetingWithCmd:ActionMeetingCmd_MuteAudio/UnMuteAudio),
// confirmed against the real header
// (ZoomSDK.framework/.../ZoomSDKMeetingActionController.h). Every method
// below therefore goes through the SAME controller getter, unlike
// TalkbackWinHost's two.

// Mirrors mac_result() in engine-talkback-sdk-macos.mm -- kept as its own
// small copy rather than shared, because that file's mapping is for the
// TalkbackSdk seam's richer operation set (create/invite/destroy/send/volume)
// and this one only ever needs actionMeetingWithCmd:'s single ZoomSDKError
// return. Sharing a two-branch switch across a new header felt like the
// wrong coupling to introduce for a mechanical port task.
static TalkbackResult mac_host_result(ZoomSDKError e, int &raw_code_out)
{
    raw_code_out = static_cast<int>(e);
    return e == ZoomSDKError_Success ? TalkbackResult::Ok : TalkbackResult::Unknown;
}

static std::string mac_host_to_utf8(NSString *s)
{
    if (!s) return {};
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

struct TalkbackMacHost::Impl {
    ZoomSDKMeetingService *svc = nil;   // not owned, see bind()
    int last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);

    ZoomSDKMeetingActionController *ctrl() const
    {
        return svc ? [svc getMeetingActionController] : nil;
    }
};

TalkbackMacHost::TalkbackMacHost() : m_impl(new Impl) {}
TalkbackMacHost::~TalkbackMacHost() = default;

void TalkbackMacHost::bind(void *zoom_meeting_service)
{
    m_impl->svc = (__bridge ZoomSDKMeetingService *)zoom_meeting_service;
}

std::vector<TalkbackParticipant> TalkbackMacHost::roster()
{
    std::vector<TalkbackParticipant> out;
    ZoomSDKMeetingActionController *c = m_impl->ctrl();
    if (!c) return out;
    NSArray *list = [c getParticipantsList];
    if (!list) return out;
    out.reserve(list.count);
    for (NSNumber *uid in list) {
        ZoomSDKUserInfo *u = [c getUserByUserID:uid.unsignedIntValue];
        if (!u) continue;
        TalkbackParticipant p;
        p.user_id = [u getUserID];
        p.display_name = mac_host_to_utf8([u getUserName]);
        // Divergence from the brief (see talkback-host.h's own comment on
        // this field): resolve_participant()'s "participant_talkback_support"
        // report line needs this per-user gate.
        p.supports_talkback = [u isSupportTalkback];
        out.push_back(std::move(p));
    }
    return out;
}

bool TalkbackMacHost::myself(TalkbackParticipant &out)
{
    ZoomSDKMeetingActionController *c = m_impl->ctrl();
    if (!c) return false;
    ZoomSDKUserInfo *u = [c getMyself];
    if (!u) return false;
    out.user_id = [u getUserID];
    out.display_name = mac_host_to_utf8([u getUserName]);
    out.supports_talkback = [u isSupportTalkback];
    return true;
}

// HAZARD: an unknown mic state must never read as "not muted" -- see
// talkback-host.h's own comment on this method, and TalkbackWinHost's
// matching one. Fails CLOSED (returns true, i.e. "muted") whenever the
// action controller or the self user cannot be resolved.
bool TalkbackMacHost::is_self_muted()
{
    ZoomSDKMeetingActionController *c = m_impl->ctrl();
    ZoomSDKUserInfo *u = c ? [c getMyself] : nil;
    if (!u) return true;
    // macOS has no IsAudioMuted() equivalent bool -- getAudioStatus() answers
    // with a status enum instead (confirmed against the real header and
    // already folded onto a bool the same way in main-macos.mm's own
    // user_to_info()). Muted/MutedByHost/MutedAllByHost are all "muted" for
    // Law 1's purposes -- what matters is whether audio the client sends
    // would currently be heard, not who muted it.
    const ZoomSDKAudioStatus audio = [u getAudioStatus];
    return audio == ZoomSDKAudioStatus_Muted ||
           audio == ZoomSDKAudioStatus_MutedByHost ||
           audio == ZoomSDKAudioStatus_MutedAllByHost;
}

TalkbackResult TalkbackMacHost::set_self_muted(bool muted)
{
    ZoomSDKMeetingActionController *c = m_impl->ctrl();
    if (!c) {
        m_impl->last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);
        return TalkbackResult::NotExist;
    }
    // userID 0 means "the current user can control the commands" (the real
    // header's own doc comment on actionMeetingWithCmd:userID:onScreen:) --
    // no getMyself() round trip needed for the mute action itself, unlike
    // Windows, whose MuteAudio/UnMuteAudio need GetMySelfUser()->GetUserID()
    // explicitly (no such sentinel exists there). ScreenType is irrelevant to
    // an audio command; ScreenType_First is the SDK's own first/default value.
    const ZoomSDKError e =
        [c actionMeetingWithCmd:(muted ? ActionMeetingCmd_MuteAudio
                                       : ActionMeetingCmd_UnMuteAudio)
                          userID:0
                        onScreen:ScreenType_First];
    return mac_host_result(e, m_impl->last_raw_code);
}

int TalkbackMacHost::last_raw_code() const { return m_impl->last_raw_code; }
