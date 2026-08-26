// tests/engine-talkback-select-test.cpp
//
// THE FIRST TEST THAT COMPILES AN ENGINE TRANSLATION UNIT.
//
// Every other talkback test in this suite compiles a pure header with src/ on
// the include path, so the engine's own call sites -- the sequence in which
// engine-talkback.cpp calls the arbiter, and what it does with the answer --
// were covered by nothing. Three separate review rounds named that gap, and
// the last Major in this feature (F1) lived precisely in it: a reviewer
// deleted the engine's generation-stamp line and all 64 tests stayed green.
// This target closes it by linking engine/src/engine-talkback.cpp itself and
// driving it through fakes for the two Zoom interfaces it touches.
//
// WHAT MAKES THAT POSSIBLE, and what it costs. IMeetingService and
// IMeetingTalkbackController are pure-abstract C++ interfaces, so the fakes
// below are ordinary subclasses and no SDK library is linked -- this test needs
// the SDK HEADERS and nothing else. The cost is that IMeetingService has 58
// pure virtuals which must all be overridden; the block in FakeMeetingService
// was generated from meeting_service_interface.h rather than typed, and an SDK
// upgrade that adds a method will break this file's COMPILE. That is the right
// failure: loud, immediate, and fixed by re-running the generator recorded in
// task-3-report.md. Do not "fix" it by weakening the fake some other way.
//
// IMeetingParticipantsController (56 pure virtuals) and IUserInfo (38) were
// deliberately NOT faked through Task 3: GetMeetingParticipantsController()
// returned nullptr, so resolve_participant() found nobody and every invite
// was skipped and reported -- exactly the "nominee not currently in the
// meeting" path, and nothing in that era's tests asserted anything about
// invites, because with no participants controller the engine could not
// issue one.
//
// Task 4 (roster re-resolution) is precisely the feature that needs a real
// roster to react to, so FakeParticipantsController/FakeUserInfo below now
// exist -- minimal fakes, generated the same way FakeMeetingService's block
// was (see above): every real SDK getter that carries state a test sets
// (name, id, IsSupportTalkback()) is hand-written, everything else is a
// default stub. FakeMeetingService's own controller starts with an EMPTY
// roster, so every test written before Task 4 is unaffected -- resolve_
// participant() still finds nobody unless a test explicitly adds someone.
//
// WHAT IT PINS: that a nomination issues exactly ONE CreateChannel at a time
// and claims the arbiter when it does; that keying issues NONE; that releasing
// a key destroys NOTHING; that a create cancelled by Leave is destroyed on
// arrival rather than adopted; and, as of Task 4, that a roster change invites
// a newly-present nominee into every channel their name is planned for, does
// so exactly once per join (idempotent under a burst of re-resolutions),
// treats TALKBACK_ERROR_ALREADY_EXIST as success rather than a reason to
// retry, and re-invites a talent who leaves and rejoins under a brand new
// user id. Those are engine-wiring facts. The pure state machine underneath
// the create/arbiter half is tested separately in
// talkback-create-state-test.cpp, which is the layer that CAN be driven
// exhaustively.
#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::test_sink() -- Task 5 fix round 3 (N6)
#include "talkback-ring.h"   // the tap side of the ring, so drain_audio() has real audio

#include <iostream>
#include <string>
#include <utility>
#include <vector>

// The SDK's types live in a namespace opened by BEGIN_ZOOM_SDK_NAMESPACE, and
// the generated override block below is copied verbatim from the header, where
// every type is written unqualified. Rather than rewrite 57 signatures (and
// have to redo it on every SDK bump), pull the namespace in here. Safe in a
// test TU with no other consumers; engine-talkback.cpp itself still qualifies
// everything as ZOOMSDK:: exactly as before.
using namespace ZOOMSDK;

// TalkbackError is nested INSIDE IMeetingTalkbackCtrlEvent, which is why
// engine-talkback.h's overrides take it unqualified -- they are members of a
// subclass. Out here it needs the enclosing class named.
static const IMeetingTalkbackCtrlEvent::TalkbackError kOk =
    IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_OK;

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// Task 5 fix round 3 (N6): substring matching, not real JSON parsing --
// matches this file's own low-tech style (no JSON library anywhere in this
// TU) and is all that is needed: report_nomination() builds these lines by
// concatenation with a fixed field order, so a substring check is exactly as
// precise as the producer's own guarantee.
static bool line_has(const std::string &line, const std::string &needle)
{
    return line.find(needle) != std::string::npos;
}

// Counts captured E2P lines matching the terminal "this nomination ladder is
// over AND it took the standing set down with it" shape --
// nomination_abort_ladder()'s report, and the ONLY thing that shape. Neither
// nominate()'s seven early refusals (never carry channels_destroyed) nor
// "nominate_done" (carries no "ok":false) can match this.
static int count_abort_reports(const std::vector<std::string> &lines)
{
    int n = 0;
    for (const auto &l : lines) {
        if (line_has(l, "\"cmd\":\"talkback_nominate\"") &&
            line_has(l, "\"stage\":\"nominate\"") &&
            line_has(l, "\"ok\":false") &&
            line_has(l, "\"channels_destroyed\":true"))
            ++n;
    }
    return n;
}

// zchar_t is wchar_t on Windows and char elsewhere, so channel ids are built a
// character at a time rather than with a literal -- the same reason
// engine-talkback.h stores them as basic_string<zchar_t>.
static std::basic_string<zchar_t> chan_id(int n)
{
    std::basic_string<zchar_t> s;
    for (const char *p = "chan-"; *p; ++p) s.push_back(static_cast<zchar_t>(*p));
    s.push_back(static_cast<zchar_t>('0' + n));
    return s;
}

static std::string utf8_of(const zchar_t *z)
{
    std::string s;
    for (; z && *z; ++z) s.push_back(static_cast<char>(*z));
    return s;
}

// Task 4's roster-driven re-resolution needs real display names to match
// against, and this file's zchar_t is wchar_t on Windows -- the same reason
// chan_id()/utf8_of() above build strings a character at a time rather than
// with a literal.
static std::basic_string<zchar_t> zstr_of(const std::string &s)
{
    std::basic_string<zchar_t> z;
    for (char c : s) z.push_back(static_cast<zchar_t>(c));
    return z;
}

// -- The fake talkback controller: a call log ------------------------------
// Records what the engine asked Zoom to do. Everything returns success, so a
// test that sees no call cannot be excused by a failure the engine handled.
class FakeTalkbackController : public ZOOMSDK::IMeetingTalkbackController {
public:
    int creates = 0;
    std::vector<std::string> destroyed;      // ids passed to AddChannelToDestroy
    std::vector<std::pair<std::string, float> > volumes;
    std::vector<std::pair<std::string, unsigned int> > sends;  // (channel, bytes)
    bool supported = true;
    // A monotonic call counter, so ORDER between two different SDK calls is
    // observable and not just their counts. Fix round 1, M4 turns on ordering
    // -- the duck must not precede the first buffer -- and a count-only fake
    // cannot tell the difference.
    int calls = 0;
    int first_send_call = -1;
    int first_volume_call = -1;

    ZOOMSDK::SDKError SetEvent(ZOOMSDK::IMeetingTalkbackCtrlEvent *) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    // Task 5 fix round 3 (N6): lets a test drive CreateChannel()'s
    // SYNCHRONOUS failure path (nomination_create_next()'s CreateChannel-!=-
    // SUCCESS abort) without a real SDK error -- the round-2 re-review's
    // mutant (c), "nothing pins the engine side". 1-based index of the
    // `creates` call to fail; -1 (default) never fails.
    int fail_create_call = -1;
    ZOOMSDK::SDKError CreateChannel(unsigned int) override
    {
        ++creates;
        if (creates == fail_create_call) return ZOOMSDK::SDKERR_UNKNOWN;
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::IMeetingTalkbackChannel *GetChannelByID(const zchar_t *) override
    { return nullptr; }
    ZOOMSDK::IList<ZOOMSDK::IMeetingTalkbackChannel *> *GetChannelList() override
    { return nullptr; }
    ZOOMSDK::SDKError BeginBatchDestroyChannels() override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError AddChannelToDestroy(const zchar_t *id) override
    { destroyed.push_back(utf8_of(id)); return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError RemoveChannelFromDestroy(const zchar_t *) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError ExecuteBatchDestroyChannels() override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    // Task 4: BEGIN/ADD/EXECUTE recorded as committed (channel, user_id)
    // pairs in `invited` -- the same Begin/Add/Execute shape `destroyed`
    // above records for the destroy side, so the roster re-resolution tests
    // can assert on invites the same way the existing tests assert on
    // destroys: by counting real SDK calls, never by parsing report output.
    std::basic_string<zchar_t> invite_channel_in_progress;
    std::vector<unsigned int> invite_users_in_progress;
    std::vector<std::pair<std::string, unsigned int> > invited;
    ZOOMSDK::SDKError BeginBatchInviteUsers(const zchar_t *channelID) override
    {
        invite_channel_in_progress = channelID;
        invite_users_in_progress.clear();
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::SDKError AddUserToInvite(unsigned int userID) override
    {
        invite_users_in_progress.push_back(userID);
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::SDKError RemoveUserFromInvite(unsigned int) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError ExecuteBatchInviteUsers() override
    {
        for (unsigned int uid : invite_users_in_progress)
            invited.push_back(std::make_pair(utf8_of(invite_channel_in_progress.c_str()), uid));
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::SDKError BeginBatchRemoveUsers(const zchar_t *) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError AddUserToRemove(unsigned int) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError RemoveUserFromRemoveList(unsigned int) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError ExecuteBatchRemoveUsers() override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError SendAudioDataToChannel(const zchar_t *id, const char *,
                                             unsigned int len, unsigned int,
                                             ZOOMSDK::ZoomSDKAudioChannel) override
    {
        if (first_send_call < 0) first_send_call = calls;
        ++calls;
        sends.push_back(std::make_pair(utf8_of(id), len));
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::SDKError SetChannelBackgroundVolume(const zchar_t *id, float v) override
    {
        if (first_volume_call < 0) first_volume_call = calls;
        ++calls;
        volumes.push_back(std::make_pair(utf8_of(id), v));
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    bool IsMeetingSupportTalkBack() override { return supported; }
};

// -- The fake participants list/user info (Task 4) -------------------------
// Only GetCount/GetItem/AddItem (IList<unsigned int>), and GetUserName/
// GetUserID/IsSupportTalkback (IUserInfo) carry real state. Everything else
// is a default stub, same convention as FakeMeetingService's generated
// block: this fake exists to drive resolve_participant() and
// resolve_roster_change(), not to be a faithful participants controller.
class FakeUIntList : public ZOOMSDK::IList<unsigned int> {
public:
    std::vector<unsigned int> items;
    int GetCount() override { return static_cast<int>(items.size()); }
    unsigned int GetItem(int index) override { return items[static_cast<size_t>(index)]; }
    void AddItem(unsigned int item) override { items.push_back(item); }
};

class FakeUserInfo : public ZOOMSDK::IUserInfo {
public:
    unsigned int id = 0;
    std::basic_string<zchar_t> name_z;
    bool supports_talkback = true;

    const zchar_t* GetUserName() override { return name_z.c_str(); }
    unsigned int GetUserID() override { return id; }
    bool IsSupportTalkback() override { return supports_talkback; }

    // -- stubs: this fake never exercises these --
    bool IsHost() override { return {}; }
    const zchar_t* GetAvatarPath() override { return {}; }
    const zchar_t* GetPersistentId() override { return {}; }
    const zchar_t* GetCustomerKey() override { return {}; }
    bool IsVideoOn() override { return {}; }
    bool IsAudioMuted() override { return {}; }
    ZOOMSDK::AudioType GetAudioJoinType() override { return {}; }
    bool IsMySelf() override { return {}; }
    bool IsInWaitingRoom() override { return {}; }
    bool IsRaiseHand() override { return {}; }
    ZOOMSDK::UserRole GetUserRole() override { return {}; }
    bool IsPurePhoneUser() override { return {}; }
    int GetAudioVoiceLevel() override { return {}; }
    bool IsClosedCaptionSender() override { return {}; }
    bool IsTalking() override { return {}; }
    bool IsH323User() override { return {}; }
    ZOOMSDK::WebinarAttendeeStatus* GetWebinarAttendeeStatus() override { return {}; }
    bool IsInterpreter() override { return {}; }
    bool IsSignLanguageInterpreter() override { return {}; }
    const zchar_t* GetInterpreterActiveLanguage() override { return {}; }
    ZOOMSDK::SDKEmojiFeedbackType GetEmojiFeedbackType() override { return {}; }
    bool IsCompanionModeUser() override { return {}; }
    ZOOMSDK::RecordingStatus GetLocalRecordingStatus() override { return {}; }
    bool IsRawLiveStreaming() override { return {}; }
    bool HasRawLiveStreamPrivilege() override { return {}; }
    bool HasCamera() override { return {}; }
    bool IsProductionStudioUser() override { return {}; }
    bool IsInWebinarBackstage() override { return {}; }
    unsigned int GetProductionStudioParent() override { return {}; }
    bool IsBotUser() override { return {}; }
    const zchar_t* GetBotAppName() override { return {}; }
    bool IsVirtualNameTagEnabled() override { return {}; }
    ZOOMSDK::IList<ZOOMSDK::ZoomSDKVirtualNameTag>* GetVirtualNameTagList() override { return {}; }
    ZOOMSDK::IList<ZOOMSDK::GrantCoOwnerAssetsInfo>* GetGrantCoOwnerAssetsInfo() override { return {}; }
    bool IsAudioOnlyUser() override { return {}; }
};

// -- The fake participants controller (Task 4) ------------------------------
// The roster this fake presents -- tests add/remove FakeUserInfo entries in
// `users` directly to simulate joins, leaves, and renames (a rename is just
// mutating name_z on an existing entry's name, exercised via remove+re-add
// below since that is how resolve_roster_change()'s generic diff sees it
// too: the old name disappears, the new one may or may not appear).
class FakeParticipantsController : public ZOOMSDK::IMeetingParticipantsController {
public:
    std::vector<FakeUserInfo> users;
    FakeUIntList ids;   // rebuilt from `users` on every GetParticipantsList()

    IList<unsigned int>* GetParticipantsList() override
    {
        ids.items.clear();
        for (auto &u : users) ids.items.push_back(u.id);
        return &ids;
    }
    IUserInfo* GetUserByUserID(unsigned int userid) override
    {
        for (auto &u : users) if (u.id == userid) return &u;
        return nullptr;
    }

    // -- stubs: this fake exists only to drive resolve_participant() and
    // resolve_roster_change() --
    SDKError SetEvent(IMeetingParticipantsCtrlEvent*) override { return {}; }
    IUserInfo* GetMySelfUser() override { return {}; }
    IUserInfo* GetBotAuthorizedUserInfoByUserID(unsigned int) override { return {}; }
    IList<unsigned int>* GetAuthorizedBotListByUserID(unsigned int) override { return {}; }
    SDKError RequestAvatarForUser(unsigned int) override { return {}; }
    IUserInfo* GetCompanionParentUser(unsigned int) override { return {}; }
    IList<unsigned int>* GetCompanionChildList(unsigned int) override { return {}; }
    SDKError LowerAllHands(bool) override { return {}; }
    SDKError ChangeUserName(const unsigned int, const zchar_t*, bool) override { return {}; }
    SDKError LowerHand(unsigned int) override { return {}; }
    SDKError RaiseHand() override { return {}; }
    SDKError MakeHost(unsigned int) override { return {}; }
    SDKError CanbeCohost(unsigned int) override { return {}; }
    SDKError AssignCoHost(unsigned int) override { return {}; }
    SDKError RevokeCoHost(unsigned int) override { return {}; }
    SDKError ExpelUser(unsigned int) override { return {}; }
    bool IsSelfOriginalHost() override { return {}; }
    SDKError ReclaimHost() override { return {}; }
    SDKError CanReclaimHost(bool&) override { return {}; }
    SDKError ReclaimHostByHostKey(const zchar_t*) override { return {}; }
    SDKError AllowParticipantsToRename(bool) override { return {}; }
    bool IsParticipantsRenameAllowed() override { return {}; }
    SDKError AllowParticipantsToUnmuteSelf(bool) override { return {}; }
    bool IsParticipantsUnmuteSelfAllowed() override { return {}; }
    SDKError AskAllToUnmute() override { return {}; }
    SDKError AllowParticipantsToStartVideo(bool) override { return {}; }
    bool IsParticipantsStartVideoAllowed() override { return {}; }
    SDKError AllowParticipantsToShareWhiteBoard(bool) override { return {}; }
    bool IsParticipantsShareWhiteBoardAllowed() override { return {}; }
    SDKError AllowParticipantsToChat(bool) override { return {}; }
    bool IsParticipantAllowedToChat() override { return {}; }
    bool IsParticipantRequestLocalRecordingAllowed() override { return {}; }
    SDKError AllowParticipantsToRequestLocalRecording(bool) override { return {}; }
    bool IsAutoAllowLocalRecordingRequest() override { return {}; }
    SDKError AutoAllowLocalRecordingRequest(bool) override { return {}; }
    SDKError CanHideParticipantProfilePictures() override { return {}; }
    bool IsParticipantProfilePicturesHidden() override { return {}; }
    SDKError HideParticipantProfilePictures(bool) override { return {}; }
    bool IsFocusModeEnabled() override { return {}; }
    bool IsFocusModeOn() override { return {}; }
    SDKError TurnFocusModeOn(bool) override { return {}; }
    FocusModeShareType GetFocusModeShareType() override { return {}; }
    SDKError SetFocusModeShareType(FocusModeShareType) override { return {}; }
    bool CanEnableParticipantRequestCloudRecording() override { return {}; }
    bool IsParticipantRequestCloudRecordingAllowed() override { return {}; }
    SDKError AllowParticipantsToRequestCloudRecording(bool) override { return {}; }
    bool IsSupportVirtualNameTag() override { return {}; }
    SDKError EnableVirtualNameTag(bool) override { return {}; }
    SDKError CreateVirtualNameTagRosterInfoBegin() override { return {}; }
    bool AddVirtualNameTagRosterInfoToList(ZoomSDKVirtualNameTag) override { return {}; }
    SDKError CreateVirtualNameTagRosterInfoCommit() override { return {}; }
    bool CanBeCoOwner(unsigned int) override { return {}; }
    SDKError AssignCoHostWithAssetsPrivilege(unsigned int, IList<GrantCoOwnerAssetsInfo>*) override { return {}; }
    SDKError MakeHostWithAssetsPrivilege(unsigned int, IList<GrantCoOwnerAssetsInfo>*) override { return {}; }
};

// -- The fake meeting service ----------------------------------------------
// Only GetMeetingTalkbackController() and GetMeetingParticipantsController()
// are written by hand. The rest is the generated block described at the top
// of this file: 57 overrides that exist solely so this class is concrete.
class FakeMeetingService : public ZOOMSDK::IMeetingService {
public:
    FakeTalkbackController ctrl;
    FakeParticipantsController participants;
    // Fix round 1, M3: simulates GetMeetingTalkbackController() returning
    // null for one call -- the meeting reconnect/ending state the review
    // named as the trigger for m_ctrl going null mid-press if
    // resolve_roster_change() ever reassigns it without an m_session_live
    // guard. Off by default so every pre-existing test is unaffected.
    bool controller_returns_null = false;
    ZOOMSDK::IMeetingTalkbackController *GetMeetingTalkbackController() override
    { return controller_returns_null ? nullptr : &ctrl; }
    ZOOMSDK::IMeetingParticipantsController *GetMeetingParticipantsController() override
    { return &participants; }

    // -- generated from meeting_service_interface.h (see the file comment) --
    SDKError SetEvent(IMeetingServiceEvent* pEvent) override { return {}; }
    SDKError HandleZoomWebUriProtocolAction(const zchar_t* protocol_action) override { return {}; }
    SDKError Join(JoinParam& joinParam) override { return {}; }
    SDKError Start(StartParam& startParam) override { return {}; }
    SDKError Leave(LeaveMeetingCmd leaveCmd) override { return {}; }
    MeetingStatus GetMeetingStatus() override { return {}; }
    SDKError LockMeeting() override { return {}; }
    SDKError UnlockMeeting() override { return {}; }
    bool IsMeetingLocked() override { return {}; }
    bool CanSetMeetingTopic() override { return {}; }
    SDKError SetMeetingTopic(const zchar_t* sTopic) override { return {}; }
    SDKError SuspendParticipantsActivities() override { return {}; }
    bool CanSuspendParticipantsActivities() override { return {}; }
    IMeetingInfo* GetMeetingInfo() override { return {}; }
    ConnectionQuality GetSharingConnQuality(bool bSending) override { return {}; }
    ConnectionQuality GetVideoConnQuality(bool bSending) override { return {}; }
    ConnectionQuality GetAudioConnQuality(bool bSending) override { return {}; }
    SDKError GetMeetingAudioStatisticInfo(MeetingAudioStatisticInfo& info) override { return {}; }
    SDKError GetMeetingVideoStatisticInfo(MeetingASVStatisticInfo& info) override { return {}; }
    SDKError GetMeetingShareStatisticInfo(MeetingASVStatisticInfo& info) override { return {}; }
    IMeetingVideoController* GetMeetingVideoController() override { return {}; }
    IMeetingShareController* GetMeetingShareController() override { return {}; }
    IMeetingAudioController* GetMeetingAudioController() override { return {}; }
    IMeetingRecordingController* GetMeetingRecordingController() override { return {}; }
    IMeetingWaitingRoomController* GetMeetingWaitingRoomController() override { return {}; }
    IMeetingWebinarController* GetMeetingWebinarController() override { return {}; }
    IMeetingRawArchivingController* GetMeetingRawArchivingController() override { return {}; }
    IMeetingReminderController* GetMeetingReminderController() override { return {}; }
    IMeetingSmartSummaryController* GetMeetingSmartSummaryController() override { return {}; }
    IMeetingChatController* GetMeetingChatController() override { return {}; }
    IMeetingBOController* GetMeetingBOController() override { return {}; }
    IMeetingConfiguration* GetMeetingConfiguration() override { return {}; }
    IMeetingAICompanionController* GetMeetingAICompanionController() override { return {}; }
    IMeetingUIController* GetUIController() override { return {}; }
    IAnnotationController* GetAnnotationController() override { return {}; }
    IMeetingRemoteController* GetMeetingRemoteController() override { return {}; }
    IMeetingH323Helper* GetH323Helper() override { return {}; }
    IMeetingPhoneHelper* GetMeetingPhoneHelper() override { return {}; }
    IMeetingLiveStreamController* GetMeetingLiveStreamController() override { return {}; }
    IClosedCaptionController* GetMeetingClosedCaptionController() override { return {}; }
    IZoomRealNameAuthMeetingHelper* GetMeetingRealNameAuthController() override { return {}; }
    IMeetingQAController* GetMeetingQAController() override { return {}; }
    IMeetingInterpretationController* GetMeetingInterpretationController() override { return {}; }
    IMeetingSignInterpretationController* GetMeetingSignInterpretationController() override { return {}; }
    IEmojiReactionController* GetMeetingEmojiReactionController() override { return {}; }
    IMeetingAANController* GetMeetingAANController() override { return {}; }
    ICustomImmersiveController* GetMeetingImmersiveController() override { return {}; }
    IMeetingWhiteboardController* GetMeetingWhiteboardController() override { return {}; }
    IMeetingDocsController* GetMeetingDocsController() override { return {}; }
    IMeetingPollingController* GetMeetingPollingController() override { return {}; }
    IMeetingRemoteSupportController* GetMeetingRemoteSupportController() override { return {}; }
    IMeetingIndicatorController* GetMeetingIndicatorController() override { return {}; }
    IMeetingProductionStudioController* GetMeetingProductionStudioController() override { return {}; }
    const zchar_t* GetInMeetingDataCenterInfo() override { return {}; }
    IMeetingEncryptionController* GetInMeetingEncryptionController() override { return {}; }
    IListFactory* GetListFactory() override { return {}; }
};

// Builds a roster entry for FakeParticipantsController::users. Every test
// below that simulates a join pushes one of these; a leave is simulated by
// removing it, and a rejoin under a NEW id (the realistic case -- ids are
// meeting-scoped and Zoom does not promise to reuse them) by pushing a fresh
// one with the same name.
static FakeUserInfo make_user(unsigned int id, const std::string &name,
                              bool supports_talkback = true)
{
    FakeUserInfo u;
    u.id = id;
    u.name_z = zstr_of(name);
    u.supports_talkback = supports_talkback;
    return u;
}

int main()
{
    // -- One CreateChannel at a time, and the arbiter is claimed -----------
    // Two nominees plan to three channels (one all-talent slice + two
    // privates), and the engine must issue them ONE AT A TIME: the arbiter
    // allows exactly one outstanding create, and CreateChannel gives no id
    // back, so two in flight cannot be told apart when the responses land.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah", "Luis"}), "nominate refused a clean two-name plan");
        check(svc.ctrl.creates == 1,
              "nominate issued more (or fewer) than one CreateChannel up front");

        // A second nomination while that create is outstanding must be
        // refused, and must not issue a create. THIS IS THE ARBITER-CLAIM
        // TEST: if the engine ever stops storing talkback_create_issued()
        // after a successful CreateChannel, nothing else in the suite
        // notices -- the pure state machine is still correct, it is just not
        // being told anything.
        check(!tb.nominate(&svc, {"Ivan"}),
              "a second nominate() was accepted while a create was outstanding");
        check(svc.ctrl.creates == 1,
              "a refused nominate() still issued a CreateChannel -- two would be "
              "outstanding at once, which the arbiter exists to prevent");

        // Drive the ladder to completion: one create per response, and not
        // one more once the plan is exhausted.
        for (int i = 1; i <= 3; ++i)
            tb.onCreateChannelResponse(chan_id(i).c_str(), kOk);
        check(svc.ctrl.creates == 3,
              "the nomination ladder did not issue exactly one CreateChannel per "
              "planned channel");
        check(svc.ctrl.destroyed.empty(),
              "a clean nomination ladder destroyed a channel");
    }

    // -- Keying SELECTS: no create, no destroy, and the whole target -------
    // Eleven nominees put all-talent over the 10-user cap, so it is two
    // channels. One key press has to reach both -- and must not create or
    // destroy anything on the way, which is the entire milestone.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        std::vector<std::string> nominees;
        for (int i = 0; i < 11; ++i) nominees.push_back("Talent " + std::to_string(i + 1));
        check(tb.nominate(&svc, nominees), "nominate refused an 11-name plan");
        // 2 all-talent + 11 private = 13 channels.
        for (int i = 1; i <= 13; ++i)
            tb.onCreateChannelResponse(chan_id(i).c_str(), kOk);
        const int creates_after_provisioning = svc.ctrl.creates;
        check(creates_after_provisioning == 13,
              "the 11-name plan did not provision 13 channels");

        // The tap's ring, built here the way TalkbackTap does, so drain_audio()
        // has something real to fan out. This is what lets the test see SENDS
        // rather than only the selection (fix round 1, M3: a `break` after the
        // first channel in send_one() previously left all 65 tests green).
        ShmRegion region{};
        const std::string region_name = "ZoomObsPluginTest_talkback_select";
        check(shm_region_create(region, region_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create a talkback ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(region.ptr), 48000, 1);

        // Fix round 2: a buffer published BEFORE the engine gets to
        // open_audio(). This is the residual window the tap's capture callback
        // opens -- it attaches as soon as TalkbackTap::open() runs, one pipe
        // write and one command-loop turn before the engine maps the region --
        // and it is the director's first syllable. open_audio() used to snap
        // the read index to the writer's current index, which stepped over it;
        // this ring is re-initialised per press, so index 0 is this press's
        // first buffer and reading from 0 is correct.
        int16_t early[480] = {0};
        early[0] = 4321;
        check(talkback_ring_publish(region.ptr, early, sizeof(early), 1),
              "the test could not publish the pre-open buffer");

        check(tb.open_audio(region_name, 48000, 1),
              "the engine refused to open the test's talkback ring");

        check(tb.session_start(&svc, kTalkbackAllTalentTarget),
              "keying the all-talent target was refused after a complete nomination");
        check(tb.session_live(), "a successful key press did not report the session live");
        check(svc.ctrl.creates == creates_after_provisioning,
              "KEYING CREATED A CHANNEL -- the create+invite round trip this "
              "milestone removed is back on the key path");
        // Fix round 1, M4: the duck must NOT run on the key press.
        // talkback_start and talkback_audio are branches of one command loop,
        // so SDK work here sits between the key going down and the first
        // buffer leaving -- the one place in this feature where work is paid
        // for in the director's first syllable. (Round 1 argued this from
        // open_audio() discarding that audio; round 2 removed the discard, so
        // the reason is the delay itself, bounded by the ring's 8 slots and
        // real loss beyond them.)
        check(svc.ctrl.volumes.empty(),
              "the key press ducked synchronously -- that SDK work sits inside the "
              "window whose audio open_audio() DISCARDS");

        // A second buffer, published after the key. Both must reach BOTH
        // channels: 2 buffers x 2 channels = 4 sends.
        int16_t pcm[480] = {0};
        pcm[0] = 1234;
        check(talkback_ring_publish(region.ptr, pcm, sizeof(pcm), 2),
              "the test could not publish a buffer into the ring");
        tb.drain_audio();

        check(svc.ctrl.sends.size() == 4,
              "ONE BUFFER DID NOT REACH EVERY CHANNEL OF THE TARGET, or the "
              "buffer published before open_audio() was DISCARDED -- an "
              "all-talent target past ten people owns several channels, and the "
              "pre-open buffer is the director's first syllable");
        if (svc.ctrl.sends.size() == 4) {
            check(svc.ctrl.sends[0].first != svc.ctrl.sends[1].first,
                  "the same channel was sent to twice instead of both channels");
            bool all_full_length = true;
            for (const auto &s : svc.ctrl.sends)
                if (s.second != sizeof(pcm)) all_full_length = false;
            check(all_full_length,
                  "the fanned-out buffers were not the buffers that were published");
        }
        // ...and only now does the duck run, after those sends.
        check(svc.ctrl.volumes.size() == 2,
              "the deferred duck never ran, so talent hear the director competing "
              "with full meeting audio for the whole press");
        check(svc.ctrl.first_send_call < svc.ctrl.first_volume_call,
              "the duck ran BEFORE the first buffer was sent -- deferring it is "
              "the point; ordering here is the whole fix");

        svc.ctrl.volumes.clear();
        tb.session_stop();
        check(!tb.session_live(), "the session stayed live after a key release");
        check(svc.ctrl.destroyed.empty(),
              "RELEASING THE KEY DESTROYED THE CHANNEL -- the next press would pay "
              "for a create+invite round trip all over again");
        check(svc.ctrl.volumes.size() == 2,
              "the key release did not restore meeting audio on both channels");

        // A second press must still work, from the same standing channels.
        check(tb.session_start(&svc, kTalkbackAllTalentTarget),
              "the second key press was refused -- the channels did not survive the first");
        check(svc.ctrl.creates == creates_after_provisioning,
              "the second key press created a channel");
        tb.session_stop();
        tb.close_audio();
        shm_region_destroy(region);
    }

    // -- A dead audio path REFUSES the key, it does not go live over it ----
    // Fix round 2 (Major). open_audio() rejects a ring it cannot use and says
    // live:false with a reason; session_start() then said live:true, and the
    // plugin's status handler is last-write-wins. With the tap opened first
    // (fix round 1) the failure arrives first and loses -- key open, OPEN cue
    // played, live tally shown, dead-man switch fresh, and nothing ever sent,
    // because drain_audio() bails on !m_audio_open. The director believes they
    // are on air. layout_mismatch is the realistic trigger: a DLL-only install,
    // which CLAUDE.md calls a routine mistake.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        ShmRegion bad{};
        const std::string bad_name = "ZoomObsPluginTest_talkback_badlayout";
        check(shm_region_create(bad, bad_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create a talkback ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(bad.ptr), 48000, 1);
        // A plugin built with a different ring layout -- i.e. half an install.
        static_cast<ShmAudioHeader *>(bad.ptr)->slot_count = kAudioRingSlots + 1;
        check(!tb.open_audio(bad_name, 48000, 1),
              "the engine accepted a ring whose layout it cannot address");

        check(!tb.session_start(&svc, "Sarah"),
              "A KEY WENT LIVE OVER A DEAD AUDIO PATH -- the director is cued, "
              "told they are live, and nothing they say ever reaches Zoom");
        check(!tb.session_live(),
              "the session reported live with the audio path rejected");
        check(svc.ctrl.sends.empty(), "a refused key sent audio");

        tb.close_audio();
        shm_region_destroy(bad);

        // ...and the refusal belongs to that attempt, not to the engine: a
        // sound ring afterwards keys normally.
        ShmRegion good{};
        const std::string good_name = "ZoomObsPluginTest_talkback_recovered";
        check(shm_region_create(good, good_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create the second ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(good.ptr), 48000, 1);
        check(tb.open_audio(good_name, 48000, 1),
              "the engine refused a sound ring after an earlier rejection");
        check(tb.session_start(&svc, "Sarah"),
              "a failed open poisoned the next press -- the reason outlived its "
              "own attempt");
        tb.session_stop();
        tb.close_audio();
        shm_region_destroy(good);
    }

    // -- Keying mid-ladder is REFUSED, not half-honoured ------------------
    // Fix round 1, M2. Provisioning is sequential -- one CreateChannel per
    // response -- so a key pressed a few hundred ms after nominate() finds
    // only part of its target's fan-out in the table. Selecting what exists so
    // far and reporting "live" put the director on air to the first ten of
    // eleven with nothing anywhere saying so. Fail closed: a refused key is
    // recoverable by pressing again, a half-broadcast is not recoverable at
    // all.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        std::vector<std::string> nominees;
        for (int i = 0; i < 11; ++i) nominees.push_back("Talent " + std::to_string(i + 1));
        check(tb.nominate(&svc, nominees), "nominate refused an 11-name plan");
        // Answer ONE create: all-talent slice 1 of 2 exists, slice 2 does not.
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);

        check(!tb.session_start(&svc, kTalkbackAllTalentTarget),
              "keying a HALF-PROVISIONED all-talent target was accepted -- the "
              "director would brief ten of eleven and be told it was live");
        check(!tb.session_live(), "a refused mid-ladder key reported the session live");

        // A target whose own channels are all present is still keyable -- the
        // refusal must be about THIS target's fan-out, not about the ladder
        // being busy. Talent 1's private channel is created second in plan
        // order (all-talent slices first, then privates).
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);   // all-talent 2/2
        tb.onCreateChannelResponse(chan_id(3).c_str(), kOk);   // Talent 1 private
        check(tb.session_start(&svc, "Talent 1"),
              "keying a fully-provisioned private target was refused just because "
              "OTHER channels were still being created");
        tb.session_stop();
    }

    // -- A redelivered nomination response must not be adopted by a probe --
    // Fix round 1, M1 (Major). The SDK redelivers onCreateChannelResponse. If
    // one arrives while a probe holds the arbiter, it is attributed to Probe,
    // skips the Nomination branch, and lands on the probe's adoption path --
    // which used to take it. The probe then invited into a talent's live
    // channel, toned at them, and destroyed it from tick().
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        check(tb.probe(&svc, "Someone"), "the probe refused to start after a nomination");
        // The SDK redelivers the response for a PROVISIONED channel while the
        // probe is waiting for its own.
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        // With the defect, the probe has adopted chan-1, failed to resolve a
        // participant (no participants controller), moved to Destroying, and
        // this tick() destroys a live talent channel.
        tb.tick();
        check(svc.ctrl.destroyed.empty(),
              "A PROBE ADOPTED AND DESTROYED A PROVISIONED CHANNEL -- talent hear "
              "a test tone and then lose the channel for the rest of the meeting");
    }

    // -- A nominee named like the all-talent sentinel is refused ------------
    // Fix round 1 (review, promoted from Minor). Display names are set by the
    // participant, and one that IS the sentinel makes keying that person's
    // name broadcast to the whole panel -- the opposite of the private aside
    // talkback exists for. Any casing, because a failure that depends on
    // capitalisation looks intermittent to an operator.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(!tb.nominate(&svc, {"Sarah", "all"}),
              "a nominee named \"all\" was nominated -- keying that name would go "
              "out to everyone");
        check(svc.ctrl.creates == 0, "a refused nomination still created a channel");
        check(!tb.nominate(&svc, {"Sarah", "All"}),
              "the sentinel collision was case-sensitive, so the failure comes and "
              "goes with a participant's capitalisation");

        // ...and the refusal leaves an existing nomination untouched.
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a clean plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);
        check(!tb.nominate(&svc, {"ALL"}), "a colliding re-nomination was accepted");
        check(svc.ctrl.destroyed.empty(),
              "a REFUSED nomination destroyed the standing channels anyway");
    }

    // -- An unprovisioned target is refused, never created on demand -------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(!tb.session_start(&svc, "Sarah"),
              "keying with nothing nominated was accepted");
        check(svc.ctrl.creates == 0,
              "keying an unprovisioned target CREATED a channel -- that is exactly "
              "the behaviour this milestone removes");
        check(!tb.session_live(), "a refused key press reported the session live");

        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        for (int i = 1; i <= 2; ++i)
            tb.onCreateChannelResponse(chan_id(i).c_str(), kOk);
        const int creates_after_provisioning = svc.ctrl.creates;
        check(!tb.session_start(&svc, "Someone Else"),
              "keying a name nobody nominated was accepted");
        check(svc.ctrl.creates == creates_after_provisioning,
              "keying an unnominated name created a channel for them");
    }

    // -- Leave with a create outstanding: destroyed on arrival, not adopted -
    // C1, the Critical of Task 2, in the engine rather than in the pure state
    // machine: nomination_reset() must leave the arbiter CLAIMED and record a
    // cancellation, so the response that is already on its way is destroyed
    // when it lands instead of being adopted (or, worse, queued as a stray
    // that nothing drains).
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        check(svc.ctrl.creates == 1, "nominate did not issue its first create");

        tb.nomination_reset();                        // what Leave/quit do
        tb.onCreateChannelResponse(chan_id(9).c_str(), kOk);
        check(svc.ctrl.destroyed.size() == 1 && svc.ctrl.destroyed[0] == "chan-9",
              "a create cancelled by Leave was not destroyed when its response "
              "arrived -- it is orphaned on Zoom and the arbiter is wedged");
        check(svc.ctrl.creates == 1,
              "the cancelled ladder carried on issuing creates after Leave");

        // And the feature is usable again afterwards: the arbiter was
        // released by that response, not left claimed forever.
        check(tb.nominate(&svc, {"Ivan"}),
              "nomination was refused after a cancelled create finally resolved");
    }

    // -- Re-nomination replaces the standing set ---------------------------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        for (int i = 1; i <= 2; ++i)
            tb.onCreateChannelResponse(chan_id(i).c_str(), kOk);
        check(svc.ctrl.destroyed.empty(), "a clean ladder destroyed something");

        check(tb.nominate(&svc, {"Ivan"}),
              "a re-nomination was refused -- the talent list would be frozen for "
              "the rest of the meeting");
        check(svc.ctrl.destroyed.size() == 2,
              "a re-nomination did not destroy the channels it replaced");
    }

    // -- Task 4: a rejoin is invited automatically, idempotently, and
    // TALKBACK_ERROR_ALREADY_EXIST is success, not failure ------------------
    // A single private nominee plans to TWO channels (one all-talent slice
    // that always exists for n>=1, plus her own private channel) -- so one
    // real join invites her into both, and `invited.size()` is the whole
    // observable signal this block needs: it must grow by exactly 2 on a
    // genuine join, stay flat across a burst of re-resolutions with nothing
    // changed, stay flat again after a mixed OK/ALREADY_EXIST confirmation,
    // and grow by another 2 on a leave-then-rejoin under a DIFFERENT user id
    // -- names, never ids, is the entire point of this milestone.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);   // all-talent
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);   // Sarah private

        // Sarah is not in the meeting yet at provisioning time -- both of
        // the initial invite attempts (in onCreateChannelResponse's own
        // invite loop) find nobody and issue no SDK call.
        check(svc.ctrl.invited.empty(),
              "an invite was issued for a nominee who was not yet in the meeting");

        // Sarah joins. This is what main.cpp's onUserJoin (etc.) calls on
        // the engine's behalf after rebuild_roster()/send_roster().
        svc.participants.users.push_back(make_user(1001, "Sarah"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "SARAH'S JOIN DID NOT INVITE HER INTO BOTH HER CHANNELS -- "
              "all-talent and her own private channel");

        // Zoom fires several of the five roster callbacks in a row for one
        // underlying change (e.g. onUserJoin then onUserAudioStatusChange).
        // Nothing changed since the last resolution -- this must invite
        // nobody again.
        tb.resolve_roster_change(&svc);
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "RE-RESOLVING WITH NOTHING CHANGED RE-INVITED -- a burst of "
              "roster callbacks for one join must do the work once, not once "
              "per callback");

        // Zoom's real answer to those two invites: ALREADY_EXIST for one
        // channel, OK for the other. TALKBACK_ERROR_ALREADY_EXIST literally
        // means "the invited user is already in the channel" -- both must be
        // treated as confirmed presence, never as a failure to retry.
        static const IMeetingTalkbackCtrlEvent::TalkbackError kAlreadyExist =
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_ALREADY_EXIST;
        tb.onChannelUserJoinResponse(chan_id(1).c_str(), 1001, kAlreadyExist);
        tb.onChannelUserJoinResponse(chan_id(2).c_str(), 1001, kOk);
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "TALKBACK_ERROR_ALREADY_EXIST WAS TREATED AS A FAILURE -- Sarah "
              "was re-invited into a channel the SDK already says she is in");
        // Fix round 1: the invite-count check above by itself is NOT enough
        // to prove ALREADY_EXIST landed in `present` rather than `failed` --
        // M1's fix (this same round) makes both suppress re-invites
        // identically, so a mutation routing ALREADY_EXIST to the failure
        // branch left the check above green. `members_present_for_target`
        // is the one place the two are NOT the same: `chan_id(1)` is the
        // all-talent channel (the ALREADY_EXIST response), and only "all"
        // matches an all-talent channel by target.
        std::size_t present = 0, total = 0;
        tb.members_present_for_target(kTalkbackAllTalentTarget, &present, &total);
        check(present == 1 && total == 1,
              "TALKBACK_ERROR_ALREADY_EXIST DID NOT MARK THE MEMBER PRESENT "
              "-- it was treated as a gate (M1's `failed`) instead of "
              "confirmed presence (`present`), which invite-count alone "
              "cannot distinguish");

        // Sarah drops (onUserLeft) and rejoins under a NEW session id
        // (onUserJoin) -- the realistic case, since ids are meeting-scoped
        // and nothing promises Zoom reuses them. She must be invited again,
        // with no operator action, resolved by NAME alone.
        svc.participants.users.clear();
        tb.resolve_roster_change(&svc);   // onUserLeft
        svc.participants.users.push_back(make_user(1002, "Sarah"));
        tb.resolve_roster_change(&svc);   // onUserJoin
        check(svc.ctrl.invited.size() == 4,
              "A REJOIN UNDER A NEW USER ID WAS NOT RE-INVITED -- resolving "
              "by name, not id, is the whole point of storing nominations as "
              "names");
    }

    // -- Task 4: a rejoiner whose client fails IsSupportTalkback() is still
    // resolved, never silently skipped ---------------------------------------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Ivan"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(
            make_user(2001, "Ivan", /*supports_talkback=*/false));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "A REJOINER WHOSE CLIENT FAILS IsSupportTalkback() WAS SILENTLY "
              "SKIPPED -- the gate is reported (resolve_participant()'s "
              "existing participant_talkback_support line), never used to "
              "quietly drop the invite");
    }

    // -- Task 4: roster re-resolution defers to a busy probe instead of
    // racing its driving thread's SDK calls, issuing neither an invite nor a
    // create -------------------------------------------------------------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        check(tb.probe(&svc, "Someone"), "the probe refused to start");
        const int creates_before_roster_event = svc.ctrl.creates;

        svc.participants.users.push_back(make_user(3001, "Sarah"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.creates == creates_before_roster_event,
              "ROSTER RE-RESOLUTION ISSUED A CreateChannel while the probe "
              "was busy");
        check(svc.ctrl.invited.empty(),
              "roster re-resolution invited while the probe's ladder was "
              "still live -- Begin/Add/Execute sequences on two threads is "
              "the exact hazard tick()'s own inventory documents");

        tb.tick();   // settle the probe so the object can be destroyed cleanly
    }

    // -- Fix round 1, M2: "never creates" pinned on the LIVE invite path,
    // not just behind the busy refusal -------------------------------------
    // The block above proves resolve_roster_change() creates nothing when it
    // does NOTHING AT ALL (refused for being busy) -- that pins "a refused
    // resolution creates nothing", not "resolution creates nothing". This
    // block runs the function to completion, with a present nominee it
    // actually invites, and checks the create counter across THAT.
    // Mutation-proved below main(): inserting a CreateChannel immediately
    // before the invite loop left the busy-path block above green while this
    // one catches it.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);
        const int creates_before_resolution = svc.ctrl.creates;

        svc.participants.users.push_back(make_user(3101, "Sarah"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "the setup for the M2 regression test did not actually invite "
              "-- this block is meaningless if nothing was invited");
        check(svc.ctrl.creates == creates_before_resolution,
              "A SUCCESSFUL ROSTER RESOLUTION THAT INVITES ALSO CREATED -- "
              "the invite-only ruling must hold on the path that actually "
              "does the work, not just behind the busy refusal");
    }

    // -- Fix round 1, C1 (CRITICAL): a leave BEFORE the invite response
    // arrives no longer wedges the rejoin, and the stale response for the
    // dead id no longer marks the new presence stint "present" --------------
    // Sequence B from the review: join -> invite -> leave before the
    // response -> rejoin under a new id. The new id's uid-based prune (the
    // fast trigger; the deadline is the backstop for when no roster event
    // ever reports the departure) must clear the stale pending entries
    // immediately on the leave event, so the rejoin invites again rather
    // than staying suppressed forever.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(make_user(4001, "Sarah"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "Sarah's join did not invite her into both her channels");

        // She leaves BEFORE either onChannelUserJoinResponse ever arrives --
        // the two pending entries for uid 4001 are still outstanding.
        svc.participants.users.clear();
        tb.resolve_roster_change(&svc);   // onUserLeft

        // She rejoins under a brand new id.
        svc.participants.users.push_back(make_user(4002, "Sarah"));
        tb.resolve_roster_change(&svc);   // onUserJoin
        check(svc.ctrl.invited.size() == 4,
              "A REJOIN AFTER AN UNANSWERED INVITE WAS PERMANENTLY SUPPRESSED "
              "-- the stale pending entries for the OLD id must be pruned the "
              "moment that id leaves the roster, not left to block the NAME "
              "forever");

        // The stale responses for the dead id (4001) finally arrive. They
        // must not be able to mark "Sarah" present -- the pending entries
        // are already gone (pruned above), so these fall through to the
        // "channel_untracked"/mismatch paths and touch nothing. If they DID
        // still match, the fresh invites issued for 4002 above would double
        // up or the count would be inconsistent; asserting the count again
        // after feeding them is the check that they were inert.
        tb.onChannelUserJoinResponse(chan_id(1).c_str(), 4001, kOk);
        tb.onChannelUserJoinResponse(chan_id(2).c_str(), 4001, kOk);
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 4,
              "A STALE RESPONSE FOR A DEAD ID RE-TRIGGERED AN INVITE OR "
              "CONFUSED THE PENDING TABLE");
    }

    // -- Fix round 2 (re-review residual 2): the DEADLINE half of C1, pinned
    // in isolation from the uid-left trigger -------------------------------
    // The re-review mutant-proved this gap: disabling ONLY the timed-out
    // check (keeping the uid-left prune) left 65/65 green, because every
    // other test that exercises expiry does so via a departure. This block
    // is the one case the review specified to close it: the uid NEVER
    // leaves the roster and no roster event ever reports it gone -- so only
    // a real deadline can ever free this name for re-invite.
    // debug_expire_pending_invites_for_test() (TEST-ONLY, guarded by
    // m_chan_mtx like every other access to the table it mutates) stands in
    // for "sleep 10 seconds", which no unit test should do for real.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);   // all-talent
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);   // Sarah private

        svc.participants.users.push_back(make_user(8001, "Sarah"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2, "Sarah's join did not invite her");

        // Neither response ever arrives -- the two pending entries are still
        // outstanding, and Sarah stays in the meeting throughout (uid 8001
        // never leaves the roster; no roster event ever reports her gone).
        tb.debug_expire_pending_invites_for_test();

        // A NO-CHANGE roster event -- same roster, nothing added or removed
        // -- is what must trigger the sweep: this pins THAT the sweep runs
        // inside resolve_roster_change() itself, not on some separate timer
        // this file does not have.
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 4,
              "THE DEADLINE-EXPIRED ENTRY WAS NEVER RE-INVITED -- with the "
              "uid still in the roster and no departure ever reported, ONLY "
              "a real timeout can free this name for re-invite, and a "
              "no-change roster event must be what runs that sweep");

        // The late responses for the ORIGINAL (now-expired-and-replaced)
        // entries finally arrive, under the SAME uid -- Sarah never
        // rejoined, so there is no "new id" to distinguish them from a
        // response to the fresh invite issued above. This is the self-heal
        // the review described (a false expiry costs one extra invite,
        // never a miscount, never a crash): the response is genuinely
        // indistinguishable from one answering the fresh invite, so it
        // legitimately confirms it -- present must land at exactly one
        // entry per channel, not zero (lost) and not two (double-counted).
        tb.onChannelUserJoinResponse(chan_id(1).c_str(), 8001, kOk);
        tb.onChannelUserJoinResponse(chan_id(2).c_str(), 8001, kOk);
        check(svc.ctrl.invited.size() == 4,
              "A LATE RESPONSE FOR AN EXPIRED ENTRY CAUSED AN EXTRA INVITE");
        std::size_t present = 0, total = 0;
        tb.members_present_for_target("Sarah", &present, &total);
        check(present == 1 && total == 1,
              "A LATE RESPONSE FOR AN EXPIRED-AND-REPLACED ENTRY WAS LOST OR "
              "DOUBLE-COUNTED -- expiry must cost at most one extra invite, "
              "never a miscount");

        // Re-resolving again with nothing changed must still be idempotent
        // -- the healed state must not itself become a source of repeated
        // invites.
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 4,
              "THE POST-EXPIRY HEALED STATE WAS NOT IDEMPOTENT");
    }

    // -- Fix round 1, M1 (Major): a permanently-failing invite is attempted
    // exactly once per presence stint, and retried only on that person's
    // next join transition -------------------------------------------------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Ivan"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(make_user(5001, "Ivan"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2, "Ivan's join did not invite him");

        // Both invites come back permanently rejected.
        static const IMeetingTalkbackCtrlEvent::TalkbackError kNoPermission =
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION;
        tb.onChannelUserJoinResponse(chan_id(1).c_str(), 5001, kNoPermission);
        tb.onChannelUserJoinResponse(chan_id(2).c_str(), 5001, kNoPermission);

        // Five more roster events for the SAME presence stint -- the shape
        // onUserAudioStatusChange/onUserVideoStatusChange produce on every
        // mute and camera toggle by anyone in the meeting, not just Ivan.
        for (int i = 0; i < 5; ++i) tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2,
              "A PERMANENTLY FAILING INVITE WAS RETRIED ON EVERY ROSTER "
              "EVENT -- a genuine gate (IsSupportTalkback() == false, most "
              "commonly) must be attempted once per presence stint, not "
              "spammed on every mute/camera toggle in the meeting");

        // He leaves and rejoins -- the one signal that plausibly changes the
        // outcome -- and gets a fresh attempt.
        svc.participants.users.clear();
        tb.resolve_roster_change(&svc);
        svc.participants.users.push_back(make_user(5002, "Ivan"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 4,
              "A REJOIN AFTER A PERMANENT FAILURE DID NOT GET A FRESH INVITE "
              "ATTEMPT");
    }

    // -- Fix round 1, M3 (Major): a roster event mid-press does not null
    // m_ctrl for the rest of the press ---------------------------------------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);

        ShmRegion region{};
        const std::string region_name = "ZoomObsPluginTest_talkback_m3";
        check(shm_region_create(region, region_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create a talkback ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(region.ptr), 48000, 1);
        check(tb.open_audio(region_name, 48000, 1),
              "the engine refused to open the test's talkback ring");
        check(tb.session_start(&svc, "Sarah"),
              "keying Sarah's private channel was refused");
        check(tb.session_live(), "the key press did not report live");

        // Simulate the reconnect/ending state the review describes:
        // GetMeetingTalkbackController() would now return null if
        // resolve_roster_change() called it.
        svc.controller_returns_null = true;
        svc.participants.users.push_back(make_user(6001, "Someone Else"));
        tb.resolve_roster_change(&svc);   // a roster event mid-press

        // m_ctrl must still be the ORIGINAL, valid controller: a buffer sent
        // now must still reach Zoom, not silently become a no_channel_drops
        // for the rest of the press.
        int16_t pcm[480] = {0};
        pcm[0] = 999;
        check(talkback_ring_publish(region.ptr, pcm, sizeof(pcm), 1),
              "the test could not publish a buffer into the ring");
        const size_t sends_before = svc.ctrl.sends.size();
        tb.drain_audio();
        check(svc.ctrl.sends.size() == sends_before + 1,
              "A ROSTER EVENT MID-PRESS NULLED m_ctrl -- the rest of the "
              "press silently stopped reaching Zoom");

        tb.session_stop();
        check(svc.ctrl.volumes.size() >= 1,
              "session_stop() could not restore the duck -- m_ctrl went null "
              "mid-press");
        tb.close_audio();
        shm_region_destroy(region);
    }

    // -- Fix round 1, M4 (Major): a CHANNEL-side leave (not a meeting leave)
    // decrements `present` and makes the person re-inviteable into THAT
    // channel -----------------------------------------------------------
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);   // all-talent
        tb.onCreateChannelResponse(chan_id(2).c_str(), kOk);   // Sarah private

        svc.participants.users.push_back(make_user(7001, "Sarah"));
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 2, "Sarah's join did not invite her");
        tb.onChannelUserJoinResponse(chan_id(1).c_str(), 7001, kOk);
        tb.onChannelUserJoinResponse(chan_id(2).c_str(), 7001, kOk);

        // Sarah stays in the MEETING throughout -- this is a channel-side
        // removal (host action / Zoom-side eviction), not a departure
        // resolve_roster_change()'s roster diff would ever see.
        tb.onChannelUserLeaveResponse(chan_id(1).c_str(), 7001, kOk);

        // Re-resolving with Sarah still in the meeting must invite her back
        // into channel 1 ONLY -- channel 2 still has her confirmed present.
        tb.resolve_roster_change(&svc);
        check(svc.ctrl.invited.size() == 3,
              "A CHANNEL-SIDE LEAVE DID NOT DECREMENT `present` -- Sarah was "
              "never re-invited into the channel she was removed from, and "
              "\"N of M present\" would overstate her membership forever");
        check(svc.ctrl.invited.back().first == utf8_of(chan_id(1).c_str()),
              "the re-invite landed on the wrong channel");

        // A leave for someone not present in a channel (already handled, or
        // a stray/duplicate response) must be a no-op, not a crash or a
        // spurious decrement.
        tb.onChannelUserLeaveResponse(chan_id(1).c_str(), 7001, kOk);
        tb.onChannelUserLeaveResponse(chan_id(9).c_str(), 9999, kOk);
    }

    // -- Task 5 fix round 3 (N6, Major): every ladder-abort path must emit
    // exactly one terminal report carrying channels_destroyed:true. The
    // round-2 re-review found this fixed on two of five structurally
    // identical branches; these three tests are the engine-side pin the
    // review named as missing entirely ("(c) nothing pins the engine side")
    // -- EngineIpc::test_sink() (engine-writer.h) is what makes a
    // report_nomination() line observable from a host test at all. ---------

    // Synchronous abort: CreateChannel() itself returns non-SUCCESS
    // (nomination_create_next()'s :1735 branch) -- the round-2 re-review's
    // mutant (c) target. Mostly validates arguments in practice, so this is
    // the LESS likely of the two nomination_create_next() aborts, but it was
    // already "fixed" with no test able to notice a regression.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.ctrl.fail_create_call = 1;   // fail the very first CreateChannel()
        check(!tb.nominate(&svc, {"Sarah"}),
              "nominate() did not report failure when CreateChannel() itself failed");
        check(count_abort_reports(lines) == 1,
              "a synchronous CreateChannel() failure did not emit exactly one "
              "terminal abort report with channels_destroyed:true");

        EngineIpc::test_sink() = nullptr;
    }

    // Async abort: the create response arrives with an error
    // (onCreateChannelResponse's channel_failed branch) -- the LIKELIER
    // real-world failure per the re-review: a genuine Zoom-side rejection
    // (budget, permission, transport) arrives here, not on CreateChannel()'s
    // synchronous return. "Sarah" plans 2 channels (all-talent + private);
    // channel 1 succeeds and is provisioned, channel 2's response fails.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        tb.onCreateChannelResponse(chan_id(1).c_str(), kOk);
        check(svc.ctrl.destroyed.empty(),
              "setup: channel 1 was destroyed before the failure even arrived");
        tb.onCreateChannelResponse(chan_id(2).c_str(),
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION);

        check(count_abort_reports(lines) == 1,
              "an async channel_failed response did not emit exactly one "
              "terminal abort report with channels_destroyed:true");
        // nomination_abort_ladder() must also have actually destroyed
        // whatever channel 1's success DID provision -- the report alone,
        // with the channel still standing, would be a lie in the other
        // direction.
        check(!svc.ctrl.destroyed.empty(),
              "channel_failed reported channels_destroyed:true but never "
              "destroyed the channel this ladder had already provisioned");

        EngineIpc::test_sink() = nullptr;
    }

    // Async abort: a swallowed create response, self-healed by a later
    // nominate() call (handle_expired_create()'s Nomination arm, via
    // debug_expire_pending_create_for_test() -- the sibling of
    // debug_expire_pending_invites_for_test(), for the OTHER expiry this
    // file has). No response is ever delivered for "Sarah"'s first channel;
    // the deadline is forced into the past, and the SECOND nominate() call's
    // own lazy self-heal is what discovers and reports the abandonment.
    {
        FakeMeetingService svc;
        EngineTalkback tb;
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&svc, {"Sarah"}), "nominate refused a one-name plan");
        check(svc.ctrl.creates == 1, "setup: the first channel's create was not issued");
        tb.debug_expire_pending_create_for_test();
        // A denominate (empty list) is enough to trigger the self-heal at
        // the top of nominate() -- it runs before this call's own plan is
        // even computed, so an empty plan afterward does not mask it.
        check(tb.nominate(&svc, {}), "an empty-list denominate was refused");

        check(count_abort_reports(lines) == 1,
              "a swallowed create response's lazy self-heal did not emit "
              "exactly one terminal abort report with channels_destroyed:true");

        EngineIpc::test_sink() = nullptr;
    }

    if (failures == 0)
        std::cout << "engine-talkback-select: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
