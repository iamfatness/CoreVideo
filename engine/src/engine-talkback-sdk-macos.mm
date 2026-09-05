#import "engine-talkback-sdk-macos.h"
#import <ZoomSDK/ZoomSDK.h>

// ZoomSDKTalkbackController.h is not imported by the ZoomSDK.h umbrella
// header directly -- it arrives transitively via ZoomSDKMeetingService.h
// (which #imports it at line 41, to declare getTalkbackController's return
// type), so the single <ZoomSDK/ZoomSDK.h> import above is sufficient.
// Recorded here because it is not obvious from this file alone.

// SDKERR_TOO_FREQUENT_CALL's Windows counterpart is documented by enum
// POSITION (18) in engine-talkback-sdk-win.h/CLAUDE.md; on macOS the real
// enum is spelled ZoomSDKError_TooFrequentCall and referenced by name for the
// same reason -- a header revision inserting a value ahead of it must not
// silently retarget Law 2's backoff.
static TalkbackResult mac_result(ZoomSDKError e, int &raw_code_out)
{
    raw_code_out = static_cast<int>(e);
    switch (e) {
    case ZoomSDKError_Success:         return TalkbackResult::Ok;
    case ZoomSDKError_TooFrequentCall: return TalkbackResult::TooFrequent;
    case ZoomSDKError_NoPermission:    return TalkbackResult::NoPermission;
    default:                           return TalkbackResult::Unknown;
    }
}

static TalkbackResult mac_tb_result(ZoomSDKTalkbackError e)
{
    switch (e) {
    case ZoomSDKTalkbackError_OK:           return TalkbackResult::Ok;
    case ZoomSDKTalkbackError_AlreadyExist: return TalkbackResult::AlreadyExists;
    case ZoomSDKTalkbackError_NoPermission: return TalkbackResult::NoPermission;
    case ZoomSDKTalkbackError_NotExist:     return TalkbackResult::NotExist;
    case ZoomSDKTalkbackError_Rejected:     return TalkbackResult::Rejected;
    case ZoomSDKTalkbackError_Timeout:      return TalkbackResult::Timeout;
    default:                                return TalkbackResult::Unknown;
    }
}

// EVERY delegate method is implemented, receive-side callbacks included.
// Divergence from the brief worth stating plainly: the real
// ZoomSDKTalkbackControllerDelegate protocol (ZoomSDKTalkbackController.h)
// declares all seven of these methods under @optional, not @required as the
// port's own history led us to expect going in -- so on THIS SDK revision an
// omitted method would not, in fact, be an unrecognized-selector crash (the
// ObjC runtime only invokes an @optional method after respondsToSelector:
// passes). Implemented anyway: the safe superset costs nothing, and nothing
// here should depend on the SDK continuing to guard every call with
// respondsToSelector: across future revisions.
@interface CVTalkbackDelegate : NSObject <ZoomSDKTalkbackControllerDelegate>
@property (nonatomic, assign) TalkbackSdkEvents *events;
@end

@implementation CVTalkbackDelegate
- (void)onCreateChannelResponse:(NSString *)channelID
                          error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_create_channel_response(
        channelID.UTF8String ?: "", mac_tb_result(error), static_cast<int>(error));
}
- (void)onDestroyChannelResponse:(NSString *)channelID
                           error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_destroy_channel_response(
        channelID.UTF8String ?: "", mac_tb_result(error), static_cast<int>(error));
}
- (void)onChannelUserJoinResponse:(NSString *)channelID
                           userID:(unsigned int)userID
                            error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_channel_user_join_response(
        channelID.UTF8String ?: "", userID, mac_tb_result(error), static_cast<int>(error));
}
- (void)onChannelUserLeaveResponse:(NSString *)channelID
                            userID:(unsigned int)userID
                             error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_channel_user_leave_response(
        channelID.UTF8String ?: "", userID, mac_tb_result(error), static_cast<int>(error));
}
// Receive-side: this engine is the DIRECTOR, never talent. Stubbed for the
// safe-superset reason above, not because they are expected to fire.
- (void)onJoinTalkbackChannel:(unsigned int)inviterID {}
- (void)onLeaveTalkbackChannel:(unsigned int)inviterID {}
- (void)onInviterAudioLevel:(unsigned int)inviterID
                 audioLevel:(unsigned int)audioLevel {}
@end

// `delegate` on ZoomSDKTalkbackController is declared `assign` (confirmed
// against the real header: `@property(nonatomic,assign,nullable)
// id<ZoomSDKTalkbackControllerDelegate> delegate;`) -- i.e. UNSAFE UNRETAINED,
// the SDK does not keep our delegate alive.
//
// Fix round (review, Minor): the previous wording here described ARC release
// semantics -- "Impl holds the only strong reference... let this go out of
// scope... a dangling pointer" -- and this target has no -fobjc-arc anywhere
// in the build (checked: neither CMakeLists.txt nor this file's own compile
// options set it), so `CVTalkbackDelegate *delegate` is a plain pointer with
// no automatic retain/release at all. Nothing in this file ever calls
// `release` on it, so the actual failure mode is the OPPOSITE of dangling:
// the object `alloc`'d in the constructor is never freed and LEAKS for the
// life of the process, rather than being released out from under a still-live
// SDK registration. The conclusion this comment exists to state is
// unchanged -- do not let anything outlive-and-call-back into a
// shorter-lived TalkbackMacSdk -- but the mechanism is a permanent
// per-instance leak (bounded: one delegate object, since this class is
// constructed once with static storage duration in production), not a
// use-after-free.
struct TalkbackMacSdk::Impl {
    ZoomSDKTalkbackController *ctrl = nil;   // not owned
    CVTalkbackDelegate *delegate = nil;      // OWNED -- see above

    // Fix-round-1-on-Windows equivalent (src/talkback-sdk.h's doc comment):
    // last_raw_code is whichever synchronous ZoomSDKError the most recent
    // controller OPERATION returned, kept only for the ladder's report
    // lines -- every ladder DECISION still keys on the TalkbackResult each
    // method returns. ZoomSDKError_UnKnown (note the capitalisation -- the
    // real enum spells it that way, not ZoomSDKError_Unknown) is the same
    // "no genuine SDK answer exists for a call that was never made" sentinel
    // TalkbackWinSdk uses for SDKERR_UNKNOWN.
    int last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);

    // events_registered mirrors Windows' post-SetEvent() bookkeeping, but
    // there is no SDKError-returning analogue here: setDelegate: is a plain
    // property setter with no failure-carrying return value at all. So
    // "registered" on macOS means exactly "bind() was last called with a
    // non-null controller", not "the SDK confirmed the registration" -- there
    // is nothing macOS could refuse this on.
    bool events_registered = false;
};

TalkbackMacSdk::TalkbackMacSdk() : m_impl(new Impl)
{
    m_impl->delegate = [[CVTalkbackDelegate alloc] init];
}
TalkbackMacSdk::~TalkbackMacSdk() = default;

void TalkbackMacSdk::bind(void *controller)
{
    // Fix round (review, Minor), left for whichever task wires this class
    // into main-macos.mm's command loop: events_registered() reads true here
    // the moment `controller` is non-null, regardless of whether
    // set_events(TalkbackSdkEvents*) (talkback-sdk.h's own interface) was
    // EVER called -- "registered" on this platform means only "setDelegate:
    // was last called with a non-null controller" (see events_registered's
    // own doc comment on the header), and CVTalkbackDelegate forwards to
    // `m_impl->delegate.events` regardless of whether that is still nullptr.
    // On Windows, TalkbackWinSdk::register_event()'s SetEvent() call and
    // set_events() are two independent calls a caller can sequence either
    // way and this same gap exists there too in principle -- but nothing
    // calls register_event() before set_events() in main.cpp today. Here,
    // with nothing wired into main-macos.mm yet, there is no caller at all to
    // check the ordering of -- so the concrete risk is only for whoever wires
    // this next: call set_events() before (or without ever skipping) bind(),
    // or a forwarding target that is silently never called (m_events null in
    // CVTalkbackDelegate, guarded by its own `if (_events)`) will look
    // identical to a healthy, registered adapter to probe()'s own
    // events_registered() refusal check.
    m_impl->ctrl = (__bridge ZoomSDKTalkbackController *)controller;
    if (m_impl->ctrl) {
        [m_impl->ctrl setDelegate:m_impl->delegate];
        m_impl->events_registered = true;
    } else {
        m_impl->events_registered = false;
    }
}

bool TalkbackMacSdk::is_meeting_support_talkback()
{
    return m_impl->ctrl && [m_impl->ctrl isMeetingSupportTalkBack];
}

TalkbackResult TalkbackMacSdk::create_channel(uint32_t count)
{
    if (!m_impl->ctrl) {
        m_impl->last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);
        return TalkbackResult::NotExist;
    }
    return mac_result([m_impl->ctrl createChannel:count], m_impl->last_raw_code);
}

TalkbackResult TalkbackMacSdk::invite_users(
    const std::string &channel_id, const std::vector<uint32_t> &user_ids)
{
    if (!m_impl->ctrl) {
        m_impl->last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);
        return TalkbackResult::NotExist;
    }
    // macOS has no Begin/Add/Execute batch API for this -- inviteUsersToChannel:
    // userIDList: is one atomic call, the whole reason the Windows batch
    // machinery (BeginBatchInviteUsers/AddUserToInvite/ExecuteBatchInviteUsers)
    // does not cross this seam.
    NSMutableArray<NSNumber *> *ids =
        [NSMutableArray arrayWithCapacity:user_ids.size()];
    for (uint32_t id : user_ids) [ids addObject:@(id)];
    return mac_result(
        [m_impl->ctrl inviteUsersToChannel:@(channel_id.c_str())
                                userIDList:ids],
        m_impl->last_raw_code);
}

TalkbackResult TalkbackMacSdk::destroy_channels(
    const std::vector<std::string> &channel_ids)
{
    if (!m_impl->ctrl) {
        m_impl->last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);
        return TalkbackResult::NotExist;
    }
    // Same single-atomic-call shape as invite_users() above.
    NSMutableArray<NSString *> *ids =
        [NSMutableArray arrayWithCapacity:channel_ids.size()];
    for (const auto &id : channel_ids) [ids addObject:@(id.c_str())];
    return mac_result([m_impl->ctrl destroyChannels:ids], m_impl->last_raw_code);
}

TalkbackResult TalkbackMacSdk::send_audio(const std::string &channel_id,
                                          const char *data, uint32_t len,
                                          uint32_t sample_rate, bool stereo)
{
    if (!m_impl->ctrl) {
        m_impl->last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);
        return TalkbackResult::NotExist;
    }
    return mac_result(
        [m_impl->ctrl sendAudioDataToChannel:@(channel_id.c_str())
                                    audioData:const_cast<char *>(data)
                                   dataLength:len
                                   sampleRate:sample_rate
                                      channel:stereo ? ZoomSDKAudioChannel_Stereo
                                                     : ZoomSDKAudioChannel_Mono],
        m_impl->last_raw_code);
}

TalkbackResult TalkbackMacSdk::set_background_volume(
    const std::string &channel_id, float volume)
{
    if (!m_impl->ctrl) {
        m_impl->last_raw_code = static_cast<int>(ZoomSDKError_UnKnown);
        return TalkbackResult::NotExist;
    }
    return mac_result(
        [m_impl->ctrl setChannelBackgroundVolume:@(channel_id.c_str())
                                 backgroundVolume:volume],
        m_impl->last_raw_code);
}

void TalkbackMacSdk::set_events(TalkbackSdkEvents *events)
{
    m_impl->delegate.events = events;
}

int TalkbackMacSdk::last_raw_code() const { return m_impl->last_raw_code; }
bool TalkbackMacSdk::events_registered() const { return m_impl->events_registered; }
