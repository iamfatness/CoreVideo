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
// WHAT IS DELIBERATELY NOT FAKED: IMeetingParticipantsController (56 pure
// virtuals) and IUserInfo (38). GetMeetingParticipantsController() returns
// nullptr, so resolve_participant() finds nobody and every invite is skipped
// and reported -- exactly the "nominee not currently in the meeting" path,
// which is a normal one. Nothing below asserts anything about invites, because
// with no participants controller the engine cannot issue one; a test that
// checked invite counts here would be asserting the fake, not the engine.
//
// WHAT IT PINS: that a nomination issues exactly ONE CreateChannel at a time
// and claims the arbiter when it does; that keying issues NONE; that releasing
// a key destroys NOTHING; and that a create cancelled by Leave is destroyed on
// arrival rather than adopted. Those are engine-wiring facts. The pure state
// machine underneath them is tested separately in talkback-create-state-test.cpp,
// which is the layer that CAN be driven exhaustively.
#include "engine-talkback.h"

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

    ZOOMSDK::SDKError SetEvent(ZOOMSDK::IMeetingTalkbackCtrlEvent *) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError CreateChannel(unsigned int) override
    { ++creates; return ZOOMSDK::SDKERR_SUCCESS; }
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
    ZOOMSDK::SDKError BeginBatchInviteUsers(const zchar_t *) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError AddUserToInvite(unsigned int) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError RemoveUserFromInvite(unsigned int) override
    { return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError ExecuteBatchInviteUsers() override
    { return ZOOMSDK::SDKERR_SUCCESS; }
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
    { sends.push_back(std::make_pair(utf8_of(id), len)); return ZOOMSDK::SDKERR_SUCCESS; }
    ZOOMSDK::SDKError SetChannelBackgroundVolume(const zchar_t *id, float v) override
    { volumes.push_back(std::make_pair(utf8_of(id), v)); return ZOOMSDK::SDKERR_SUCCESS; }
    bool IsMeetingSupportTalkBack() override { return supported; }
};

// -- The fake meeting service ----------------------------------------------
// Only GetMeetingTalkbackController() is written by hand. The rest is the
// generated block described at the top of this file: 57 overrides that exist
// solely so this class is concrete.
class FakeMeetingService : public ZOOMSDK::IMeetingService {
public:
    FakeTalkbackController ctrl;
    ZOOMSDK::IMeetingTalkbackController *GetMeetingTalkbackController() override
    { return &ctrl; }

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
    IMeetingParticipantsController* GetMeetingParticipantsController() override { return {}; }
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

        check(tb.session_start(&svc, kTalkbackAllTalentTarget),
              "keying the all-talent target was refused after a complete nomination");
        check(tb.session_live(), "a successful key press did not report the session live");
        check(svc.ctrl.creates == creates_after_provisioning,
              "KEYING CREATED A CHANNEL -- the create+invite round trip this "
              "milestone removed is back on the key path");
        check(svc.ctrl.volumes.size() == 2,
              "keying all-talent did not reach both of its channels -- everyone "
              "past the tenth person hears silence");

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

    if (failures == 0)
        std::cout << "engine-talkback-select: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
