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

// LIVE GATE RUN 1: the same shape as count_abort_reports() but pinned to one
// reason string, so "the ladder aborted" and "the ladder aborted FOR THE
// REASON THE OPERATOR NEEDS TO SEE" are separate assertions. The rate-limit
// abort exists precisely because a generic create failure sent the first live
// gate hunting permissions and channel budget for a problem that was neither.
static int count_abort_reports_because(const std::vector<std::string> &lines,
                                       const std::string &reason)
{
    int n = 0;
    for (const auto &l : lines) {
        if (line_has(l, "\"cmd\":\"talkback_nominate\"") &&
            line_has(l, "\"stage\":\"nominate\"") &&
            line_has(l, "\"ok\":false") &&
            line_has(l, "\"channels_destroyed\":true") &&
            line_has(l, "\"reason\":\"" + reason + "\""))
            ++n;
    }
    return n;
}

// LIVE GATE RUN 1: the ladder's SUCCESSFUL terminal. Needed to prove a
// rate-limited ladder does not merely avoid aborting but actually finishes --
// "no abort" alone is also true of a ladder that quietly stopped.
static int count_done_reports(const std::vector<std::string> &lines)
{
    int n = 0;
    for (const auto &l : lines)
        if (line_has(l, "\"cmd\":\"talkback_nominate\"") &&
            line_has(l, "\"stage\":\"nominate_done\""))
            ++n;
    return n;
}

// Final review, C2: counts the ONE line that tells the plugin a session it
// believes is live is over -- report_session_state(false, reason). Shape-
// matched the same low-tech way count_abort_reports() is: no "stage" key, a
// top-level "live" key, which is exactly how ZoomEngineClient::handle_event()
// tells this line from a session stage trace.
static int count_session_dead_reports(const std::vector<std::string> &lines,
                                      const std::string &reason)
{
    int n = 0;
    for (const auto &l : lines) {
        if (line_has(l, "\"cmd\":\"talkback_session\"") &&
            line_has(l, "\"live\":false") &&
            line_has(l, "\"reason\":\"" + reason + "\""))
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

// -- The fake talkback SDK: a call log --------------------------------------
// Records what the engine asked the seam to do. Everything returns Ok, so a
// test that sees no call cannot be excused by a failure the engine handled.
//
// Task 1 (2026-09-04): this used to be FakeTalkbackController, subclassing
// ZOOMSDK::IMeetingTalkbackController directly. It now implements TalkbackSdk
// (src/talkback-sdk.h) instead -- the whole point of the macOS-port seam is
// that this fake no longer needs a single Zoom type or a raw SDKError to
// drive the engine's talkback ladder. Every recorded field below is
// unchanged in MEANING from the pre-Task-1 fake; only the interface it is
// recorded through changed.
class FakeTalkbackSdk : public TalkbackSdk {
public:
    int creates = 0;
    std::vector<std::string> destroyed;      // ids destroy_channels() was given
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

    void set_events(TalkbackSdkEvents *) override {}
    // Task 5 fix round 3 (N6): lets a test drive CreateChannel()'s
    // SYNCHRONOUS failure path (nomination_create_next()'s create-!=-Ok
    // abort) without a real SDK error -- the round-2 re-review's mutant (c),
    // "nothing pins the engine side". 1-based index of the `creates` call to
    // fail; -1 (default) never fails.
    int fail_create_call = -1;
    // LIVE GATE RUN 1 (2026-08-26): the rate limit a real Zoom has and this
    // fake never did. `rate_limit_next` is how many UPCOMING create_channel()
    // calls answer TalkbackResult::TooFrequent (Windows spells this
    // SDKERR_TOO_FREQUENT_CALL, enum position 18) -- the code the live gate
    // saw when the ladder issued channel 2 from inside channel 1's response,
    // 0ms apart. `rate_limited` counts how many actually were, so a test
    // cannot mistake "the engine never called" for "the call was refused".
    int rate_limit_next = 0;
    int rate_limited = 0;
    // TALKBACK DELIVERY LAW 2 (ZComms, 2026-08-29): the SAME rate limit, on
    // INVITES. This is the half no fake in this file had, and the half our
    // engine had no handling for: `invite_rate_limit_next` is how many
    // upcoming invite_users() calls answer TalkbackResult::TooFrequent,
    // `invite_rate_limited` counts how many actually did (so a test cannot
    // mistake "the engine never called" for "the call was refused").
    int invite_rate_limit_next = 0;
    int invite_rate_limited = 0;
    // Task 1, Step 2's pin (the seam's TalkbackResult normalisation reaching
    // the ladder's own retry decision): a FIFO of results for successive
    // create_channel() calls, checked BEFORE fail_create_call/rate_limit_next
    // so it is a strict addition -- every pre-existing test leaves this
    // empty and is unaffected. `create_calls()` is the same count as
    // `creates`, exposed as a method because the brief's Step 2 test reads it
    // that way.
    std::vector<TalkbackResult> scripted_create_results;
    void script_create_results(std::vector<TalkbackResult> results)
    { scripted_create_results = std::move(results); }
    int create_calls() const { return creates; }

    TalkbackResult create_channel(uint32_t) override
    {
        ++creates;
        if (!scripted_create_results.empty()) {
            const TalkbackResult r = scripted_create_results.front();
            scripted_create_results.erase(scripted_create_results.begin());
            return record(r);
        }
        if (creates == fail_create_call) return record(TalkbackResult::Unknown);
        if (rate_limit_next > 0) {
            --rate_limit_next;
            ++rate_limited;
            return record(TalkbackResult::TooFrequent);
        }
        return record(TalkbackResult::Ok);
    }
    TalkbackResult destroy_channels(const std::vector<std::string> &channel_ids) override
    {
        // Task 1: destroy_channels() is a single call over a whole list
        // (Begin/Add/ExecuteBatchDestroyChannels collapsed by the seam), but
        // every real call site in this codebase passes at most one id -- see
        // engine-talkback-sdk-win.h's own comment on why the batch shape
        // lives there and nowhere above it. Recorded exactly like the old
        // fake's AddChannelToDestroy(), one push per id.
        for (const auto &id : channel_ids) destroyed.push_back(id);
        return record(TalkbackResult::Ok);
    }
    // Task 4: BEGIN/ADD/EXECUTE recorded as committed (channel, user_id)
    // pairs in `invited` -- the same shape `destroyed` above records for the
    // destroy side, so the roster re-resolution tests can assert on invites
    // the same way the existing tests assert on destroys: by counting real
    // SDK calls, never by parsing report output.
    std::vector<std::pair<std::string, unsigned int> > invited;
    // 2026-08-29: ordering marker, the same device `first_send_call` and
    // `first_volume_call` already are. The neutral background-volume set has
    // to land BEFORE anybody is invited into the channel, or a member spends
    // the gap at Zoom's ducked default -- which is the whole defect. A count
    // of invites cannot see that; a call index can.
    int first_invite_call = -1;
    TalkbackResult invite_users(const std::string &channel_id,
                                const std::vector<uint32_t> &user_ids) override
    {
        if (first_invite_call < 0) first_invite_call = calls;
        ++calls;
        // LAW 2: refuse BEFORE recording, so a refused invite does not appear
        // in `invited` -- Zoom did not accept it, and a test that counted it
        // would be asserting on a membership that does not exist.
        if (invite_rate_limit_next > 0) {
            --invite_rate_limit_next;
            ++invite_rate_limited;
            return record(TalkbackResult::TooFrequent);
        }
        for (uint32_t uid : user_ids)
            invited.push_back(std::make_pair(channel_id, uid));
        return record(TalkbackResult::Ok);
    }
    TalkbackResult send_audio(const std::string &channel_id, const char *,
                              uint32_t len, uint32_t, bool) override
    {
        if (first_send_call < 0) first_send_call = calls;
        ++calls;
        sends.push_back(std::make_pair(channel_id, len));
        return record(TalkbackResult::Ok);
    }
    TalkbackResult set_background_volume(const std::string &channel_id, float v) override
    {
        if (first_volume_call < 0) first_volume_call = calls;
        ++calls;
        volumes.push_back(std::make_pair(channel_id, v));
        return record(TalkbackResult::Ok);
    }
    bool is_meeting_support_talkback() override { return supported; }

    // Fix round 1 (Findings 2 & 3). This fake has no real platform
    // underneath it, so there is no genuine raw SDK code to surface --
    // last_raw_code() mirrors the most recent TalkbackResult numerically
    // (via record(), called by every operation above), which is honest
    // about what it is (nothing asserts a specific value against it; the
    // real diagnostics CLAUDE.md documents are about TalkbackWinSdk's
    // mapping, verified there by inspection, not about this fake). Defaults
    // to true so every pre-existing probe()-driving test, none of which
    // simulates a failed event registration, is unaffected.
    int last_raw_code() const override { return m_last_code; }
    bool events_registered_value = true;
    bool events_registered() const override { return events_registered_value; }

private:
    TalkbackResult record(TalkbackResult r) { m_last_code = static_cast<int>(r); return r; }
    int m_last_code = 0;
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
    // TALKBACK DELIVERY LAW 1 (ZComms, 2026-08-29): real state, not a stub.
    // This is the AUTHORITATIVE read of "is this client's meeting audio open"
    // -- IMeetingAudioController has no such getter anywhere in
    // meeting_audio_interface.h, only MuteAudio/UnMuteAudio -- so it is what
    // ensure_mic_open() consults, and therefore what a test has to be able to
    // set. Defaults to false (open) so every pre-Law-1 test in this file sees
    // an already-unmuted client and issues no audio-controller call at all.
    bool muted = false;
    bool IsAudioMuted() override { return muted; }
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

// -- The fake audio controller (LAW 1, 2026-08-29) --------------------------
// Talkback delivers ONLY while this client's own meeting audio is open, so a
// key press unmutes and a key release restores. Two facts have to be
// observable for that to be pinnable at all: WHETHER the calls happened, and
// WHEN they happened relative to the first SendAudioDataToChannel -- "unmute
// before the first send" is the whole law, and a pair of counters cannot see
// ordering.
//
// So this fake borrows FakeTalkbackSdk's monotonic `calls` clock rather
// than keeping one of its own: the two controllers are different objects but
// the ordering question spans both, and two independent counters cannot be
// compared. `clock` is wired by FakeMeetingService's constructor.
class FakeAudioController : public ZOOMSDK::IMeetingAudioController {
public:
    FakeTalkbackSdk *clock = nullptr;
    std::vector<unsigned int> unmuted;   // user ids passed to UnMuteAudio
    std::vector<unsigned int> muted;     // user ids passed to MuteAudio
    int first_unmute_call = -1;
    // Lets a test drive a meeting that FORBIDS self-unmute -- the "some
    // meetings lock mute" case, which must leave the key live and reported
    // "mic":"blocked" rather than silently pretending the mic is open.
    ZOOMSDK::SDKError unmute_result = ZOOMSDK::SDKERR_SUCCESS;

    ZOOMSDK::SDKError UnMuteAudio(unsigned int userid) override
    {
        if (clock) {
            if (first_unmute_call < 0) first_unmute_call = clock->calls;
            ++clock->calls;
        }
        if (unmute_result == ZOOMSDK::SDKERR_SUCCESS) unmuted.push_back(userid);
        return unmute_result;
    }
    ZOOMSDK::SDKError MuteAudio(unsigned int userid, bool) override
    {
        if (clock) ++clock->calls;
        muted.push_back(userid);
        return ZOOMSDK::SDKERR_SUCCESS;
    }

    // -- stubs --
    ZOOMSDK::SDKError SetEvent(ZOOMSDK::IMeetingAudioCtrlEvent *) override { return {}; }
    ZOOMSDK::SDKError JoinVoip() override { return {}; }
    ZOOMSDK::SDKError LeaveVoip() override { return {}; }
    bool CanUnMuteBySelf() override { return {}; }
    bool CanEnableMuteOnEntry() override { return {}; }
    ZOOMSDK::SDKError EnableMuteOnEntry(bool, bool) override { return {}; }
    bool IsMuteOnEntryEnabled() override { return {}; }
    ZOOMSDK::SDKError EnablePlayChimeWhenEnterOrExit(bool) override { return {}; }
    ZOOMSDK::SDKError StopIncomingAudio(bool) override { return {}; }
    bool IsIncomingAudioStopped() override { return {}; }
    bool Is3rdPartyTelephonyAudioOn() override { return {}; }
    ZOOMSDK::SDKError EnablePlayMeetingAudio(bool) override { return {}; }
    bool IsPlayMeetingAudioEnabled() override { return {}; }
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

    // LAW 1 (2026-08-29): this client's OWN entry, which is what
    // ensure_mic_open() reads IsAudioMuted() from and unmutes by id. Kept
    // SEPARATE from `users` on purpose: the engine is not a nominee, it never
    // appears in a talkback plan, and putting it in the roster would silently
    // change what resolve_roster_change() diffs in every pre-existing test.
    // `has_self` off by default so tests written before Law 1 see
    // GetMySelfUser() == nullptr exactly as they always did -- which
    // ensure_mic_open() reports and treats as "cannot open", never as "open".
    bool has_self = false;
    FakeUserInfo self;

    // -- stubs: this fake exists only to drive resolve_participant() and
    // resolve_roster_change() --
    SDKError SetEvent(IMeetingParticipantsCtrlEvent*) override { return {}; }
    IUserInfo* GetMySelfUser() override { return has_self ? &self : nullptr; }
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
// GetMeetingTalkbackController() and GetMeetingParticipantsController() are
// written by hand. The rest is the generated block described at the top of
// this file: 57 overrides that exist solely so this class is concrete.
class FakeMeetingService : public ZOOMSDK::IMeetingService {
public:
    // Task 1 (2026-09-04): FakeTalkbackSdk (formerly FakeTalkbackController)
    // no longer implements ZOOMSDK::IMeetingTalkbackController, so it can no
    // longer be returned from GetMeetingTalkbackController() -- see that
    // method's own removal below. Kept as a member (not hoisted out to every
    // test's own local) purely so every existing test's `svc.ctrl.xxx`
    // read/write keeps compiling unchanged; a test now must ALSO call
    // `tb.set_sdk(&svc.ctrl)` for the engine to actually reach it (every test
    // in this file does, right after declaring `tb`).
    FakeTalkbackSdk ctrl;
    FakeParticipantsController participants;
    // LAW 1 (2026-08-29). Wired to the talkback controller's monotonic call
    // clock in the constructor, so "the unmute happened before the first
    // send" is one comparison across two controllers -- see
    // FakeAudioController.
    FakeAudioController audio;
    FakeMeetingService() { audio.clock = &ctrl; }
    // Fix round 1, M3's test toggle used to simulate
    // GetMeetingTalkbackController() returning null for one call -- the
    // meeting reconnect/ending state the review named as the trigger for
    // m_ctrl going null mid-press if resolve_roster_change() ever reassigned
    // it without an m_session_live guard. Task 1 REMOVED that internal
    // reassignment entirely (see EngineTalkback::set_sdk()'s comment): the
    // adapter is now injected by the caller and never re-derived from this
    // service, so the failure mode this toggle simulated is structurally
    // unreachable any more -- nothing reads this field. Left in place
    // (inert) so the one test that sets it (search `controller_returns_null`)
    // still compiles and still documents the invariant it was written for.
    bool controller_returns_null = false;
    // IMeetingService's pure virtual, still required to make this class
    // concrete. Task 1: this fake's `ctrl` is TalkbackSdk-typed now, not
    // ZOOMSDK::IMeetingTalkbackController*, so it can no longer be returned
    // here -- and nothing in engine-talkback.cpp calls this method any more
    // (the adapter is injected via EngineTalkback::set_sdk() instead), so
    // returning null unconditionally changes nothing observable.
    ZOOMSDK::IMeetingTalkbackController *GetMeetingTalkbackController() override
    { return nullptr; }
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
    IMeetingAudioController* GetMeetingAudioController() override { return &audio; }
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

// -- The fake TalkbackHost (Task 2b, 2026-09-05) ----------------------------
// Mirrors TalkbackWinHost (engine/src/engine-talkback-host-win.h) against
// these same fakes rather than the real SDK: the roster walk goes through
// FakeParticipantsController exactly as resolve_participant()/
// current_roster() do in production, and the mute calls go through
// FakeAudioController exactly as ensure_mic_open()/restore_mic_state() do.
// Every test in this file constructs one alongside its FakeMeetingService and
// injects both (`tb.set_sdk(&svc.ctrl); tb.set_host(&host);`), mirroring
// main.cpp's own paired injection.
class FakeTalkbackHost : public TalkbackHost {
public:
    explicit FakeTalkbackHost(FakeMeetingService &svc) : m_svc(&svc) {}

    std::vector<TalkbackParticipant> roster() override
    {
        std::vector<TalkbackParticipant> out;
        IList<unsigned int> *ids = m_svc->participants.GetParticipantsList();
        if (!ids) return out;
        for (int i = 0; i < ids->GetCount(); ++i) {
            const unsigned int uid = ids->GetItem(i);
            IUserInfo *u = m_svc->participants.GetUserByUserID(uid);
            if (!u) continue;
            TalkbackParticipant p;
            p.user_id = uid;
            p.display_name = utf8_of(u->GetUserName());
            p.supports_talkback = u->IsSupportTalkback();
            out.push_back(std::move(p));
        }
        return out;
    }

    bool myself(TalkbackParticipant &out) override
    {
        IUserInfo *self = m_svc->participants.GetMySelfUser();
        if (!self) return false;
        out.user_id = self->GetUserID();
        out.display_name = utf8_of(self->GetUserName());
        out.supports_talkback = self->IsSupportTalkback();
        return true;
    }

    // HAZARD (this task's brief): an unknown mic state must never read as
    // "not muted". Fails CLOSED (true, i.e. "muted") when there is no self
    // user to ask, mirroring TalkbackWinHost's own is_self_muted().
    bool is_self_muted() override
    {
        IUserInfo *self = m_svc->participants.GetMySelfUser();
        if (!self) return true;
        return self->IsAudioMuted();
    }

    // Step 11 (this task's brief): lets a test drive set_self_muted()
    // FAILING without touching FakeAudioController's own unmute_result (which
    // simulates a real SDK refusal, not a missing controller) -- the
    // mutation-provable invariant is "an unmute the seam itself cannot
    // complete still leaves m_mic_open false", distinct from "Zoom refused
    // the unmute", which FakeAudioController::unmute_result already covers.
    bool fail_set_self_muted = false;

    TalkbackResult set_self_muted(bool muted) override
    {
        if (fail_set_self_muted) {
            m_last_raw_code = -1;
            return TalkbackResult::Unknown;
        }
        IUserInfo *self = m_svc->participants.GetMySelfUser();
        if (!self) {
            m_last_raw_code = -1;
            return TalkbackResult::NotExist;
        }
        const ZOOMSDK::SDKError e = muted
            ? m_svc->audio.MuteAudio(self->GetUserID(), true)
            : m_svc->audio.UnMuteAudio(self->GetUserID());
        m_last_raw_code = static_cast<int>(e);
        return e == ZOOMSDK::SDKERR_SUCCESS ? TalkbackResult::Ok : TalkbackResult::Unknown;
    }

    int last_raw_code() const override { return m_last_raw_code; }

private:
    FakeMeetingService *m_svc;
    int m_last_raw_code = -1;
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

// LIVE GATE RUN 1 (2026-08-26): delivers a create response the way the ENGINE
// now sees one, and every existing test in this file calls it instead of
// onCreateChannelResponse directly.
//
// Why it exists: the ladder no longer issues channel N+1 from inside channel
// N's response -- Zoom refused that with SDKERR_TOO_FREQUENT_CALL (18) in the
// live gate -- it schedules it kMembershipCallSpacing (600ms; was kNominationCreateSpacing at 300ms before the ZComms per-call measurement) later and
// nomination_tick() issues it from the command loop. A test that only
// delivered responses would provision channel 1 and then stop. The two extra
// calls here are the test's stand-in for 300ms of command-loop idle time:
// expire the spacing deadline, then pump. Both are no-ops when nothing is
// scheduled, which is why the non-ladder call sites (probe responses,
// redelivered duplicates, untracked extras) can use this helper unchanged.
//
// Tests that assert on the PACING ITSELF deliberately do NOT use this -- they
// call onCreateChannelResponse and nomination_tick() separately, because the
// fact being pinned is precisely that the create does not happen in between.
// TALKBACK DELIVERY LAW 2 (2026-08-29): the same stand-in, for the pacer's
// INVITE half. Invites are no longer issued inline by
// onCreateChannelResponse's member loop or by resolve_roster_change() -- they
// are queued, and nomination_tick() spends ONE membership call per
// kMembershipCallSpacing (600ms) on creates and invites together, because the
// rate limit ZComms measured counts calls, not call kinds.
//
// So every test that asserts on invite COUNTS needs the test's stand-in for
// however many 600ms of command-loop idle the queue is worth. This runs the
// pump to quiescence. The bound is a bound, not an expected iteration count:
// this file's biggest plan is 13 channels and 22 invites, and the loop is
// idempotent once the queue is empty (nomination_tick() with nothing due is
// one mutex acquire and a compare).
//
// WHAT THIS DOES NOT WEAKEN: the counts every pre-existing assertion checks
// are UNCHANGED by Law 2, because a drained queue issues exactly the invites
// the old inline code issued. What changed is only WHEN, which is what the
// dedicated pacing tests below assert on directly and deliberately do not use
// this for.
static void drain_membership(EngineTalkback &tb)
{
    for (int i = 0; i < 128; ++i) {
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
    }
}

// Task 2b: the test-side mirror of engine-talkback-sdk-win.h's
// talkback_win_tb_result() -- this file drives EngineTalkback's own
// on_*_response() methods directly now (it implements TalkbackSdkEvents, not
// ZOOMSDK::IMeetingTalkbackCtrlEvent, any more), so something here has to do
// the same TalkbackError->TalkbackResult translation the real adapter does in
// production. Deliberately not #including engine-talkback-sdk-win.h to reuse
// its version: that header also declares TalkbackWinSdk's full
// ZOOMSDK::IMeetingTalkbackCtrlEvent override set, which would make this TU
// implicitly depend on the real controller's SetEvent() shape matching too --
// a coupling this file's own fakes exist to avoid. Only Ok/AlreadyExist are
// ever compared against by the ladder (see that function's own comment for
// why every other value maps to Unknown); the raw `err` is threaded through
// separately as `raw_code` regardless, so no test assertion depends on this
// mapping being any richer.
static TalkbackResult test_tb_result(IMeetingTalkbackCtrlEvent::TalkbackError err)
{
    switch (err) {
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_OK: return TalkbackResult::Ok;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_ALREADY_EXIST:
        return TalkbackResult::AlreadyExists;
    default: return TalkbackResult::Unknown;
    }
}

// Task 2b: thin shims preserving every existing call site's argument shape
// (a zchar_t* channel id, a bare TalkbackError) while calling EngineTalkback's
// renamed, portable methods -- on_create_channel_response()/
// on_channel_user_join_response()/on_channel_user_leave_response(), which take
// (UTF-8 string, TalkbackResult, raw_code) instead of
// (const zchar_t*, TalkbackError). Mechanical translation only: every value
// a test already passes (chan_id(N).c_str(), a user id, kOk/kAlreadyExist/
// kNoPermission/...) is unchanged.
static void tb_create_response(EngineTalkback &tb, const zchar_t *id,
                               IMeetingTalkbackCtrlEvent::TalkbackError err)
{
    tb.on_create_channel_response(utf8_of(id), test_tb_result(err),
                                  static_cast<int>(err));
}
static void tb_join_response(EngineTalkback &tb, const zchar_t *id, unsigned int user_id,
                            IMeetingTalkbackCtrlEvent::TalkbackError err)
{
    tb.on_channel_user_join_response(utf8_of(id), user_id, test_tb_result(err),
                                     static_cast<int>(err));
}
static void tb_leave_response(EngineTalkback &tb, const zchar_t *id, unsigned int user_id,
                             IMeetingTalkbackCtrlEvent::TalkbackError err)
{
    tb.on_channel_user_leave_response(utf8_of(id), user_id, test_tb_result(err),
                                      static_cast<int>(err));
}

static void respond(EngineTalkback &tb, const zchar_t *id,
                    IMeetingTalkbackCtrlEvent::TalkbackError err)
{
    tb_create_response(tb, id, err);
    drain_membership(tb);
}

// LAW 2: resolve_roster_change() decides WHO needs inviting and the pacer
// decides WHEN, so a test that wants to see the invites has to let the pacer
// run. Every call site that asserts on invites goes through this.
//
// Task 2b: takes FakeTalkbackHost now instead of ZOOMSDK::IMeetingService --
// resolve_roster_change() itself takes TalkbackHost*.
static void resolve(EngineTalkback &tb, FakeTalkbackHost &host)
{
    tb.resolve_roster_change(&host);
    drain_membership(tb);
}

// LIVE PRODUCTION 2026-08-29: every channel is set to NEUTRAL background
// volume the moment it is created, because Zoom's own default for a channel
// member is ducked -- talent lost meeting audio by being ASSIGNED, before any
// key was pressed. These two helpers let a test say "exactly one neutral set
// per provisioned channel, and nothing else" without hand-rolling the count.
static bool volume_is(float v, float expected)
{
    return v > expected - 0.001f && v < expected + 0.001f;
}

// Counts entries in the fake's volume log that set `expected` on any channel.
static int count_volume_sets(const std::vector<std::pair<std::string, float> > &volumes,
                             float expected)
{
    int n = 0;
    for (const auto &v : volumes)
        if (volume_is(v.second, expected)) ++n;
    return n;
}

// True when EVERY id in the log is distinct -- "exactly one per channel" is
// two facts (the right count, and no channel set twice while another was
// missed) and a bare count proves only the first.
static bool volume_channels_distinct(
    const std::vector<std::pair<std::string, float> > &volumes)
{
    for (std::size_t i = 0; i < volumes.size(); ++i)
        for (std::size_t j = i + 1; j < volumes.size(); ++j)
            if (volumes[i].first == volumes[j].first) return false;
    return true;
}

// Wipes the volume log AND the ordering marker, so a test that cares about the
// KEYED cycle starts from the same blank slate it had before provisioning set
// anything. Without the first_volume_call reset, the provision-time neutral
// set would make "the duck ran after the first send" trivially false.
static void reset_volume_log(FakeTalkbackSdk &ctrl)
{
    ctrl.volumes.clear();
    ctrl.first_volume_call = -1;
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a clean two-name plan");
        check(svc.ctrl.creates == 1,
              "nominate issued more (or fewer) than one CreateChannel up front");

        // A second nomination while that create is outstanding must be
        // refused, and must not issue a create. THIS IS THE ARBITER-CLAIM
        // TEST: if the engine ever stops storing talkback_create_issued()
        // after a successful CreateChannel, nothing else in the suite
        // notices -- the pure state machine is still correct, it is just not
        // being told anything.
        check(!tb.nominate(&host, {"Ivan"}),
              "a second nominate() was accepted while a create was outstanding");
        check(svc.ctrl.creates == 1,
              "a refused nominate() still issued a CreateChannel -- two would be "
              "outstanding at once, which the arbiter exists to prevent");

        // Drive the ladder to completion: one create per response, and not
        // one more once the plan is exhausted.
        for (int i = 1; i <= 3; ++i)
            respond(tb, chan_id(i).c_str(), kOk);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> nominees;
        for (int i = 0; i < 11; ++i) nominees.push_back("Talent " + std::to_string(i + 1));
        check(tb.nominate(&host, nominees), "nominate refused an 11-name plan");
        // 2 all-talent + 11 private = 13 channels.
        for (int i = 1; i <= 13; ++i)
            respond(tb, chan_id(i).c_str(), kOk);
        const int creates_after_provisioning = svc.ctrl.creates;
        check(creates_after_provisioning == 13,
              "the 11-name plan did not provision 13 channels");

        // LIVE PRODUCTION 2026-08-29: provisioning leaves every channel
        // explicitly NEUTRAL. Asserted here as well as in its own block below
        // because this is the test that then drives the keyed cycle -- the two
        // halves of "idle is neutral, keyed is ducked, idle is neutral again"
        // have to be pinned against the SAME channel set to mean anything.
        check(static_cast<int>(svc.ctrl.volumes.size()) == 13,
              "provisioning did not set background volume once per channel");
        check(count_volume_sets(svc.ctrl.volumes, 1.0f) == 13,
              "PROVISIONING LEFT ZOOM'S DEFAULT IN PLACE -- talent are ducked "
              "by being ASSIGNED to a channel, before any key is pressed");
        check(volume_channels_distinct(svc.ctrl.volumes),
              "the neutral set did not reach every channel exactly once");
        // From here the test is about the KEYED cycle, so start it from a
        // blank log -- see reset_volume_log().
        reset_volume_log(svc.ctrl);

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

        check(tb.session_start(&host, kTalkbackAllTalentTarget),
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
        check(count_volume_sets(svc.ctrl.volumes, 0.3f) == 2,
              "the key-down duck did not set the ducked level on both channels");
        check(svc.ctrl.first_send_call < svc.ctrl.first_volume_call,
              "the duck ran BEFORE the first buffer was sent -- deferring it is "
              "the point; ordering here is the whole fix");

        reset_volume_log(svc.ctrl);
        tb.session_stop();
        check(!tb.session_live(), "the session stayed live after a key release");
        check(svc.ctrl.destroyed.empty(),
              "RELEASING THE KEY DESTROYED THE CHANNEL -- the next press would pay "
              "for a create+invite round trip all over again");
        check(svc.ctrl.volumes.size() == 2,
              "the key release did not restore meeting audio on both channels");
        // 2026-08-29: the restore must write the SAME neutral the provision
        // wrote, not a value cached from before the duck -- Zoom's default is
        // itself ducked, so a "restore what it was" would hand talent back the
        // duck and make idle-after-a-key differ from idle-before-the-first.
        check(count_volume_sets(svc.ctrl.volumes, 1.0f) == 2,
              "THE KEY RELEASE RESTORED SOMETHING OTHER THAN NEUTRAL -- idle "
              "after a press must be identical to idle before the first one");

        // A second press must still work, from the same standing channels.
        check(tb.session_start(&host, kTalkbackAllTalentTarget),
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

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

        check(!tb.session_start(&host, "Sarah"),
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
        check(tb.session_start(&host, "Sarah"),
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> nominees;
        for (int i = 0; i < 11; ++i) nominees.push_back("Talent " + std::to_string(i + 1));
        check(tb.nominate(&host, nominees), "nominate refused an 11-name plan");
        // Answer ONE create: all-talent slice 1 of 2 exists, slice 2 does not.
        respond(tb, chan_id(1).c_str(), kOk);

        check(!tb.session_start(&host, kTalkbackAllTalentTarget),
              "keying a HALF-PROVISIONED all-talent target was accepted -- the "
              "director would brief ten of eleven and be told it was live");
        check(!tb.session_live(), "a refused mid-ladder key reported the session live");

        // A target whose own channels are all present is still keyable -- the
        // refusal must be about THIS target's fan-out, not about the ladder
        // being busy. Talent 1's private channel is created second in plan
        // order (all-talent slices first, then privates).
        respond(tb, chan_id(2).c_str(), kOk);   // all-talent 2/2
        respond(tb, chan_id(3).c_str(), kOk);   // Talent 1 private
        check(tb.session_start(&host, "Talent 1"),
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        check(tb.probe(&host, "Someone"), "the probe refused to start after a nomination");
        // The SDK redelivers the response for a PROVISIONED channel while the
        // probe is waiting for its own.
        respond(tb, chan_id(1).c_str(), kOk);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(!tb.nominate(&host, {"Sarah", "all"}),
              "a nominee named \"all\" was nominated -- keying that name would go "
              "out to everyone");
        check(svc.ctrl.creates == 0, "a refused nomination still created a channel");
        check(!tb.nominate(&host, {"Sarah", "All"}),
              "the sentinel collision was case-sensitive, so the failure comes and "
              "goes with a participant's capitalisation");

        // ...and the refusal leaves an existing nomination untouched.
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a clean plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);
        check(!tb.nominate(&host, {"ALL"}), "a colliding re-nomination was accepted");
        check(svc.ctrl.destroyed.empty(),
              "a REFUSED nomination destroyed the standing channels anyway");
    }

    // -- An unprovisioned target is refused, never created on demand -------
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(!tb.session_start(&host, "Sarah"),
              "keying with nothing nominated was accepted");
        check(svc.ctrl.creates == 0,
              "keying an unprovisioned target CREATED a channel -- that is exactly "
              "the behaviour this milestone removes");
        check(!tb.session_live(), "a refused key press reported the session live");

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        for (int i = 1; i <= 2; ++i)
            respond(tb, chan_id(i).c_str(), kOk);
        const int creates_after_provisioning = svc.ctrl.creates;
        check(!tb.session_start(&host, "Someone Else"),
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        check(svc.ctrl.creates == 1, "nominate did not issue its first create");

        tb.nomination_reset();                        // what Leave/quit do
        respond(tb, chan_id(9).c_str(), kOk);
        check(svc.ctrl.destroyed.size() == 1 && svc.ctrl.destroyed[0] == "chan-9",
              "a create cancelled by Leave was not destroyed when its response "
              "arrived -- it is orphaned on Zoom and the arbiter is wedged");
        check(svc.ctrl.creates == 1,
              "the cancelled ladder carried on issuing creates after Leave");

        // And the feature is usable again afterwards: the arbiter was
        // released by that response, not left claimed forever.
        check(tb.nominate(&host, {"Ivan"}),
              "nomination was refused after a cancelled create finally resolved");
    }

    // -- Re-nomination replaces the standing set ---------------------------
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        for (int i = 1; i <= 2; ++i)
            respond(tb, chan_id(i).c_str(), kOk);
        check(svc.ctrl.destroyed.empty(), "a clean ladder destroyed something");

        check(tb.nominate(&host, {"Ivan"}),
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);   // all-talent
        respond(tb, chan_id(2).c_str(), kOk);   // Sarah private

        // Sarah is not in the meeting yet at provisioning time -- both of
        // the initial invite attempts (in onCreateChannelResponse's own
        // invite loop) find nobody and issue no SDK call.
        check(svc.ctrl.invited.empty(),
              "an invite was issued for a nominee who was not yet in the meeting");

        // Sarah joins. This is what main.cpp's onUserJoin (etc.) calls on
        // the engine's behalf after rebuild_roster()/send_roster().
        svc.participants.users.push_back(make_user(1001, "Sarah"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 2,
              "SARAH'S JOIN DID NOT INVITE HER INTO BOTH HER CHANNELS -- "
              "all-talent and her own private channel");

        // Zoom fires several of the five roster callbacks in a row for one
        // underlying change (e.g. onUserJoin then onUserAudioStatusChange).
        // Nothing changed since the last resolution -- this must invite
        // nobody again.
        resolve(tb, host);
        resolve(tb, host);
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
        tb_join_response(tb, chan_id(1).c_str(), 1001, kAlreadyExist);
        tb_join_response(tb, chan_id(2).c_str(), 1001, kOk);
        resolve(tb, host);
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
        resolve(tb, host);   // onUserLeft
        svc.participants.users.push_back(make_user(1002, "Sarah"));
        resolve(tb, host);   // onUserJoin
        check(svc.ctrl.invited.size() == 4,
              "A REJOIN UNDER A NEW USER ID WAS NOT RE-INVITED -- resolving "
              "by name, not id, is the whole point of storing nominations as "
              "names");
    }

    // -- Task 4: a rejoiner whose client fails IsSupportTalkback() is still
    // resolved, never silently skipped ---------------------------------------
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Ivan"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(
            make_user(2001, "Ivan", /*supports_talkback=*/false));
        resolve(tb, host);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        check(tb.probe(&host, "Someone"), "the probe refused to start");
        const int creates_before_roster_event = svc.ctrl.creates;

        svc.participants.users.push_back(make_user(3001, "Sarah"));
        resolve(tb, host);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);
        const int creates_before_resolution = svc.ctrl.creates;

        svc.participants.users.push_back(make_user(3101, "Sarah"));
        resolve(tb, host);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(make_user(4001, "Sarah"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 2,
              "Sarah's join did not invite her into both her channels");

        // She leaves BEFORE either onChannelUserJoinResponse ever arrives --
        // the two pending entries for uid 4001 are still outstanding.
        svc.participants.users.clear();
        resolve(tb, host);   // onUserLeft

        // She rejoins under a brand new id.
        svc.participants.users.push_back(make_user(4002, "Sarah"));
        resolve(tb, host);   // onUserJoin
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
        tb_join_response(tb, chan_id(1).c_str(), 4001, kOk);
        tb_join_response(tb, chan_id(2).c_str(), 4001, kOk);
        resolve(tb, host);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);   // all-talent
        respond(tb, chan_id(2).c_str(), kOk);   // Sarah private

        svc.participants.users.push_back(make_user(8001, "Sarah"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 2, "Sarah's join did not invite her");

        // Neither response ever arrives -- the two pending entries are still
        // outstanding, and Sarah stays in the meeting throughout (uid 8001
        // never leaves the roster; no roster event ever reports her gone).
        tb.debug_expire_pending_invites_for_test();

        // A NO-CHANGE roster event -- same roster, nothing added or removed
        // -- is what must trigger the sweep: this pins THAT the sweep runs
        // inside resolve_roster_change() itself, not on some separate timer
        // this file does not have.
        resolve(tb, host);
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
        tb_join_response(tb, chan_id(1).c_str(), 8001, kOk);
        tb_join_response(tb, chan_id(2).c_str(), 8001, kOk);
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
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 4,
              "THE POST-EXPIRY HEALED STATE WAS NOT IDEMPOTENT");
    }

    // -- Fix round 1, M1 (Major): a permanently-failing invite is attempted
    // exactly once per presence stint, and retried only on that person's
    // next join transition -------------------------------------------------
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Ivan"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(make_user(5001, "Ivan"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 2, "Ivan's join did not invite him");

        // Both invites come back permanently rejected.
        static const IMeetingTalkbackCtrlEvent::TalkbackError kNoPermission =
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION;
        tb_join_response(tb, chan_id(1).c_str(), 5001, kNoPermission);
        tb_join_response(tb, chan_id(2).c_str(), 5001, kNoPermission);

        // Five more roster events for the SAME presence stint -- the shape
        // onUserAudioStatusChange/onUserVideoStatusChange produce on every
        // mute and camera toggle by anyone in the meeting, not just Ivan.
        for (int i = 0; i < 5; ++i) resolve(tb, host);
        check(svc.ctrl.invited.size() == 2,
              "A PERMANENTLY FAILING INVITE WAS RETRIED ON EVERY ROSTER "
              "EVENT -- a genuine gate (IsSupportTalkback() == false, most "
              "commonly) must be attempted once per presence stint, not "
              "spammed on every mute/camera toggle in the meeting");

        // He leaves and rejoins -- the one signal that plausibly changes the
        // outcome -- and gets a fresh attempt.
        svc.participants.users.clear();
        resolve(tb, host);
        svc.participants.users.push_back(make_user(5002, "Ivan"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 4,
              "A REJOIN AFTER A PERMANENT FAILURE DID NOT GET A FRESH INVITE "
              "ATTEMPT");
    }

    // -- Fix round 1, M3 (Major): a roster event mid-press does not null
    // m_ctrl for the rest of the press ---------------------------------------
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        ShmRegion region{};
        const std::string region_name = "ZoomObsPluginTest_talkback_m3";
        check(shm_region_create(region, region_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create a talkback ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(region.ptr), 48000, 1);
        check(tb.open_audio(region_name, 48000, 1),
              "the engine refused to open the test's talkback ring");
        check(tb.session_start(&host, "Sarah"),
              "keying Sarah's private channel was refused");
        check(tb.session_live(), "the key press did not report live");

        // Simulate the reconnect/ending state the review describes. Task 1:
        // `controller_returns_null` is now INERT -- resolve_roster_change()
        // no longer calls GetMeetingTalkbackController() at all, in either of
        // its branches (see EngineTalkback::set_sdk()'s comment), so this
        // line no longer causes anything different to happen. Left set
        // anyway, so this test still documents the scenario it was written
        // against; the assertions below now hold for a stronger reason than
        // before -- there is no internal re-derivation left to race at all,
        // not merely one that is guarded correctly.
        svc.controller_returns_null = true;
        svc.participants.users.push_back(make_user(6001, "Someone Else"));
        resolve(tb, host);   // a roster event mid-press

        // m_sdk must still be the ORIGINAL, valid adapter: a buffer sent
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

        // 2026-08-29: this used to be `>= 1`, which provisioning's own neutral
        // sets would now satisfy on their own -- the assertion would have
        // stayed green with the restore deleted. Clear the log first so the
        // only thing that can satisfy it is session_stop()'s own call.
        reset_volume_log(svc.ctrl);
        tb.session_stop();
        check(count_volume_sets(svc.ctrl.volumes, 1.0f) == 1,
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);   // all-talent
        respond(tb, chan_id(2).c_str(), kOk);   // Sarah private

        svc.participants.users.push_back(make_user(7001, "Sarah"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 2, "Sarah's join did not invite her");
        tb_join_response(tb, chan_id(1).c_str(), 7001, kOk);
        tb_join_response(tb, chan_id(2).c_str(), 7001, kOk);

        // Sarah stays in the MEETING throughout -- this is a channel-side
        // removal (host action / Zoom-side eviction), not a departure
        // resolve_roster_change()'s roster diff would ever see.
        tb_leave_response(tb, chan_id(1).c_str(), 7001, kOk);

        // Re-resolving with Sarah still in the meeting must invite her back
        // into channel 1 ONLY -- channel 2 still has her confirmed present.
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 3,
              "A CHANNEL-SIDE LEAVE DID NOT DECREMENT `present` -- Sarah was "
              "never re-invited into the channel she was removed from, and "
              "\"N of M present\" would overstate her membership forever");
        check(svc.ctrl.invited.back().first == utf8_of(chan_id(1).c_str()),
              "the re-invite landed on the wrong channel");

        // A leave for someone not present in a channel (already handled, or
        // a stray/duplicate response) must be a no-op, not a crash or a
        // spurious decrement.
        tb_leave_response(tb, chan_id(1).c_str(), 7001, kOk);
        tb_leave_response(tb, chan_id(9).c_str(), 9999, kOk);
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.ctrl.fail_create_call = 1;   // fail the very first CreateChannel()
        check(!tb.nominate(&host, {"Sarah"}),
              "nominate() did not report failure when CreateChannel() itself failed");
        check(count_abort_reports(lines) == 1,
              "a synchronous CreateChannel() failure did not emit exactly one "
              "terminal abort report with channels_destroyed:true");

        // LIVE GATE RUN 1, test (d): the rate-limit retry added below must not
        // swallow the OTHER synchronous failures. Only SDKERR_TOO_FREQUENT_CALL
        // is a "not yet"; SDKERR_UNKNOWN and everything like it still end the
        // ladder on the first try, with the generic reason. Pumping afterwards
        // proves no retry was quietly armed for the pump to pick up.
        check(count_abort_reports_because(lines, "create_channel_failed") == 1,
              "a non-rate-limit CreateChannel() failure did not abort with its "
              "own reason");
        for (int i = 0; i < 5; ++i) {
            tb.debug_expire_create_spacing_for_test();
            tb.nomination_tick();
        }
        check(svc.ctrl.creates == 1,
              "A NON-RATE-LIMIT CreateChannel() FAILURE WAS RETRIED -- the "
              "backoff is for SDKERR_TOO_FREQUENT_CALL and nothing else");
        check(count_abort_reports(lines) == 1,
              "the pump produced a second terminal report for a ladder that had "
              "already aborted");

        EngineIpc::test_sink() = nullptr;
    }

    // -- LIVE GATE RUN 1 (2026-08-26): THE LADDER IS PACED, AND A RATE-LIMIT
    // REFUSAL IS A WAIT, NOT A FAILURE ------------------------------------
    //
    // In the first live gate against a real meeting, a one-nominee nomination
    // planned 2 channels. Channel 1 was created and its nominee invited; then
    // the ladder issued channel 2's CreateChannel synchronously from inside
    // channel 1's onCreateChannelResponse -- both lines timestamped
    // 20:04:37.291, a 0ms gap -- and Zoom answered SDKERR_TOO_FREQUENT_CALL
    // (18). The ladder aborted terminally, correctly by its own rules and
    // uselessly in practice: every real talent list plans more than one
    // channel, so NO nomination could ever have succeeded live. Nothing in
    // this suite could have caught it -- the fake controller had no rate limit
    // and no notion of elapsed time between calls.
    //
    // (a) The spacing itself: create N+1 is not issued from inside channel N's
    // response, and not before its deadline either. This is the one test in
    // the file that must NOT use respond() -- the fact being pinned is
    // precisely what happens in between.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        // "Sarah" plans 2 channels: all-talent + her private.
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        check(svc.ctrl.creates == 1, "setup: the first create was not issued");

        tb_create_response(tb, chan_id(1).c_str(), kOk);
        check(svc.ctrl.creates == 1,
              "THE LIVE GATE DEFECT: channel 2's CreateChannel was issued from "
              "inside channel 1's onCreateChannelResponse, 0ms after it -- Zoom "
              "refuses that with SDKERR_TOO_FREQUENT_CALL");

        // The pump runs constantly (every ~50ms of command-loop idle); it must
        // do nothing until the spacing deadline has actually passed.
        tb.nomination_tick();
        check(svc.ctrl.creates == 1,
              "the pump issued the next create before its spacing deadline -- "
              "the deadline is the whole mechanism");

        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.creates == 2,
              "the pump never issued the next create once its deadline passed "
              "-- the ladder would stall forever after channel 1");
    }

    // (b) A code-18 refusal backs off and retries THE SAME CHANNEL, and the
    // ladder completes on the retry. This is the mutation-proved one: make the
    // 18 branch fall through to the generic abort (i.e. delete the retry) and
    // the "no abort" and "completed" checks below both fail.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        tb_create_response(tb, chan_id(1).c_str(), kOk);   // schedules channel 2

        // Zoom refuses exactly the next create -- the live gate's shape, only
        // now with 300ms of spacing already spent and Zoom still saying "not
        // yet". That is the case the backoff exists for.
        svc.ctrl.rate_limit_next = 1;
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.rate_limited == 1,
              "setup: the fake never refused a create, so this test proves "
              "nothing about the rate limit");
        check(count_abort_reports(lines) == 0,
              "A RATE-LIMITED CREATE ENDED THE LADDER -- this is the live gate "
              "defect itself: SDKERR_TOO_FREQUENT_CALL is Zoom saying 'not "
              "yet', and treating it as a failure means no multi-channel "
              "nomination can ever succeed");

        // The retry, after the backoff. Same channel: the plan's front was
        // never popped, so this is channel 2 again, not channel 3.
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.creates == 3,
              "the rate-limited create was never retried -- the ladder is "
              "stalled with no terminal report at all, the worst of both");

        respond(tb, chan_id(2).c_str(), kOk);
        check(count_done_reports(lines) == 1,
              "the ladder did not reach its ONE successful terminal after "
              "riding out a rate limit");
        check(count_abort_reports(lines) == 0,
              "a ladder that completed on a retry also reported an abort -- "
              "every ladder exit must reach exactly one terminal");
        check(svc.ctrl.destroyed.empty(),
              "a ladder that completed on a retry still tore its channels down");

        EngineIpc::test_sink() = nullptr;
    }

    // (c) Retries exhausted: exactly one terminal abort, and its reason names
    // the rate limit rather than blaming CreateChannel generically -- the
    // operator has to learn the true cause from the log, which is the whole
    // reason this reason string exists.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.ctrl.rate_limit_next = 100;   // Zoom never lets up
        // TRUE, not false: a rate-limited FIRST create leaves the ladder alive
        // with a retry armed, so nominate() must still assign its plan -- see
        // nomination_create_next()'s declaration comment. A false here would
        // mean the retry fires against an empty queue.
        check(tb.nominate(&host, {"Sarah"}),
              "nominate() gave up on the first create instead of arming a retry");

        // More pumps than the cap, so "stopped retrying" is observable rather
        // than merely "ran out of test".
        for (int i = 0; i < 12; ++i) {
            tb.debug_expire_create_spacing_for_test();
            tb.nomination_tick();
        }
        // kMaxNominationCreateRetries is 4 (file-local to engine-talkback.cpp),
        // so: the initial create plus four retries, then the abort.
        check(svc.ctrl.creates == 5,
              "the rate-limit retry is not capped at kMaxNominationCreateRetries "
              "-- an unbounded retry is a ladder that never reports at all");
        check(count_abort_reports(lines) == 1,
              "exhausted rate-limit retries did not emit EXACTLY ONE terminal "
              "abort with channels_destroyed:true");
        check(count_abort_reports_because(lines, "create_rate_limited") == 1,
              "the exhausted-retry abort did not name the rate limit -- a "
              "generic create failure sends the operator hunting permissions "
              "and channel budget for a problem that is neither");

        EngineIpc::test_sink() = nullptr;
    }

    // (e) THE WINDOW THE PACING OPENS, and the gate that closes it. Found by
    // mutation, not by review: deleting the scheduled-create half of
    // nominate()'s gate left every test above green.
    //
    // Before the ladder was paced, "the arbiter is free" and "no ladder is
    // mid-provisioning" were the same fact -- the next create left from inside
    // the previous response, so there was no instant in between, and
    // nominate()'s arbiter gate refused every mid-ladder re-nomination for
    // free. kMembershipCallSpacing opens ~600ms per rung where the arbiter
    // is genuinely free and a ladder is genuinely still running. A
    // re-nomination landing there would pass the gate, run its replace path,
    // destroy the running ladder's channels and start a second ladder over the
    // top -- and the FIRST ladder would end with no terminal report of its
    // own, which is the one rule this feature's whole abort machinery exists
    // to hold (Task 5 fix rounds 2 and 3 are both about exactly that).
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        // Channel 1 lands: the arbiter is released by the response, and
        // channel 2 is scheduled but not yet issued. This is the window.
        tb_create_response(tb, chan_id(1).c_str(), kOk);

        check(!tb.nominate(&host, {"Ivan"}),
              "A RE-NOMINATION INSIDE THE LADDER'S SPACING WINDOW WAS ACCEPTED "
              "-- it would destroy the running ladder's channels and leave that "
              "ladder with no terminal report at all");
        check(svc.ctrl.creates == 1,
              "the refused re-nomination still issued a CreateChannel");
        check(svc.ctrl.destroyed.empty(),
              "the refused re-nomination tore down the running ladder's "
              "channels -- nominate()'s early gate must destroy nothing");

        // ...and the original ladder is untouched: it resumes on the next pump
        // and reaches exactly one terminal, its own.
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.creates == 2,
              "the refused re-nomination stalled the ladder it was refused for");
        respond(tb, chan_id(2).c_str(), kOk);
        check(count_done_reports(lines) == 1,
              "the ladder that survived a refused re-nomination did not reach "
              "exactly one successful terminal");
        check(count_abort_reports(lines) == 0,
              "a refused re-nomination made the running ladder abort");

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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        check(svc.ctrl.destroyed.empty(),
              "setup: channel 1 was destroyed before the failure even arrived");
        respond(tb, chan_id(2).c_str(),
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
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        check(svc.ctrl.creates == 1, "setup: the first channel's create was not issued");
        tb.debug_expire_pending_create_for_test();
        // A denominate (empty list) is enough to trigger the self-heal at
        // the top of nominate() -- it runs before this call's own plan is
        // even computed, so an empty plan afterward does not mask it.
        check(tb.nominate(&host, {}), "an empty-list denominate was refused");

        check(count_abort_reports(lines) == 1,
              "a swallowed create response's lazy self-heal did not emit "
              "exactly one terminal abort report with channels_destroyed:true");

        EngineIpc::test_sink() = nullptr;
    }

    // -- FINAL REVIEW, C1 (CRITICAL): the attempt id an operator's request
    // carries must survive the whole ladder, and a REFUSAL must carry its
    // OWN id, not the running ladder's ------------------------------------
    //
    // This is the engine half of the fix. The plugin stages one attempt at a
    // time (src/talkback-nomination.h); without an echoed id, a nomination
    // sent mid-ladder overwrote the running ladder's staging and the running
    // ladder's own nominate_done then committed the WRONG nominee list. The
    // engine's job is to make the two attempts distinguishable on the wire.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        // Ladder A: attempt 99, two channels (all-talent + Sarah private).
        check(tb.nominate(&host, {"Sarah"}, 99), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);

        // Attempt 100 arrives MID-LADDER and is refused at the arbiter gate
        // with "create_busy" -- the engine's documented, correct behaviour,
        // and the exact interleaving C1 rides in on. Its refusal must carry
        // 100, never 99: reporting the running ladder's id for a refusal is
        // the confusion the id exists to remove.
        check(!tb.nominate(&host, {"Dave"}, 100),
              "a re-nomination mid-ladder was accepted");
        bool refusal_tagged_100 = false;
        for (const auto &l : lines)
            if (line_has(l, "\"reason\":\"create_busy\"") && line_has(l, "\"attempt\":100"))
                refusal_tagged_100 = true;
        check(refusal_tagged_100,
              "C1: a mid-ladder refusal did not carry ITS OWN attempt id -- the "
              "plugin cannot tell it apart from a terminal for the ladder that "
              "is still running");

        // The STAGING stage lines carry it too, not just the terminals: two
        // nominates can sit in the pipe before this engine reads the first,
        // so the plugin can already have staged the second when the first's
        // stage lines arrive. Unidentified, those fold one attempt's
        // shortfall names into the other attempt's record -- and
        // uncovered_private is read by talkback_target_known_unprovisioned(),
        // so a spurious name there refuses a key on a standing channel.
        bool plan_tagged_99 = false;
        for (const auto &l : lines)
            if (line_has(l, "\"stage\":\"plan\"") && line_has(l, "\"attempt\":99"))
                plan_tagged_99 = true;
        check(plan_tagged_99,
              "C1: a STAGING stage line carried no attempt id -- the plugin "
              "stages it into whatever slot happens to be current");

        // Ladder A finishes. nominate_done must still be tagged 99.
        respond(tb, chan_id(2).c_str(), kOk);
        bool done_tagged_99 = false;
        for (const auto &l : lines)
            if (line_has(l, "\"stage\":\"nominate_done\"") && line_has(l, "\"attempt\":99"))
                done_tagged_99 = true;
        check(done_tagged_99,
              "C1: THE ATTEMPT ID DID NOT SURVIVE THE LADDER -- nominate_done "
              "for attempt 99 arrived unidentified, so the plugin would commit "
              "it against whatever attempt is staged now");

        EngineIpc::test_sink() = nullptr;
    }

    // The id survives an ABORT too -- the terminal a failed ladder emits is
    // the one the plugin acts on most destructively (it resets the confirmed
    // plan), so it is the one that must not be attributed to the wrong
    // attempt.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        check(tb.nominate(&host, {"Sarah"}, 43), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(),
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION);

        check(count_abort_reports(lines) == 1,
              "setup: the failing ladder did not emit its terminal abort report");
        bool abort_tagged_43 = false;
        for (const auto &l : lines)
            if (line_has(l, "\"channels_destroyed\":true") && line_has(l, "\"attempt\":43"))
                abort_tagged_43 = true;
        check(abort_tagged_43,
              "C1: the ladder's ABORT report lost the attempt id -- the plugin "
              "would reset the confirmed plan on behalf of an attempt that may "
              "already have been superseded");

        EngineIpc::test_sink() = nullptr;
    }

    // -- FINAL REVIEW, C2 (CRITICAL): a ladder abort that destroys the
    // channels a LIVE key is talking on must UN-LIVE that session ---------
    //
    // Keying "all" mid-ladder is legal and deliberate: session_start() gates
    // only on `still_coming` for ITS OWN target, so once both all-talent
    // slices exist the key goes live while the private channels are still
    // being created. A later create failure -- CLAUDE.md's own "LIKELIER
    // real-world failure ... budget past 16 channels, permission, transport"
    // -- then batch-destroys every channel underneath that press. Before this
    // fix nothing reported it: m_session_live stayed true, the plugin's
    // session status stayed live, the tally stayed red, the OPEN cue had
    // already played, and not one sample reached Zoom.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        std::vector<std::string> nominees;
        for (int i = 0; i < 11; ++i) nominees.push_back("Talent " + std::to_string(i + 1));
        check(tb.nominate(&host, nominees), "nominate refused an 11-name plan");
        // 2 all-talent slices + 11 privates = 13. Answer only the two slices:
        // "all" is fully provisioned, the ladder is still running.
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        ShmRegion region{};
        const std::string region_name = "ZoomObsPluginTest_talkback_c2";
        check(shm_region_create(region, region_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create a talkback ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(region.ptr), 48000, 1);
        check(tb.open_audio(region_name, 48000, 1),
              "the engine refused to open the test's talkback ring");
        check(tb.session_start(&host, kTalkbackAllTalentTarget),
              "keying a fully-provisioned all-talent target mid-ladder was "
              "refused -- that refusal would deny a ready target for an "
              "unrelated reason");
        check(tb.session_live(), "setup: the key press did not report live");

        int16_t pcm[480] = {0};
        pcm[0] = 1234;
        check(talkback_ring_publish(region.ptr, pcm, sizeof(pcm), 1),
              "the test could not publish a buffer into the ring");
        tb.drain_audio();
        const std::size_t sends_while_live = svc.ctrl.sends.size();
        check(sends_while_live == 2,
              "setup: the live key did not fan out to both all-talent channels");

        // Create #3 (the first private) is rejected by Zoom.
        respond(tb, chan_id(3).c_str(),
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION);

        check(count_abort_reports(lines) == 1,
              "the ladder abort did not emit exactly one terminal abort report");
        check(count_session_dead_reports(lines, "channels_destroyed") == 1,
              "C2: THE CHANNELS A LIVE KEY WAS TALKING ON WERE DESTROYED AND "
              "NOTHING TOLD THE PLUGIN -- the key stays open, the tally stays "
              "red, the OPEN cue already played, and the director is off air "
              "believing they are on it");
        check(!tb.session_live(),
              "C2: the engine still believes the session is live after its "
              "channels were destroyed");

        // ...and it actually STOPPED. A session that reports dead but keeps
        // sending would just be the same lie from the other end.
        check(talkback_ring_publish(region.ptr, pcm, sizeof(pcm), 2),
              "the test could not publish a second buffer");
        tb.drain_audio();
        check(svc.ctrl.sends.size() == sends_while_live,
              "C2: audio was still sent after the session's channels were "
              "destroyed");

        tb.close_audio();
        shm_region_destroy(region);
        EngineIpc::test_sink() = nullptr;
    }

    // -- FINAL REVIEW, M1 (Major): `present` is pruned by USER ID, so a
    // leave+rejoin under a new id is re-invited -----------------------------
    //
    // Channel membership is per user id; a rejoin gets a NEW one and is NOT
    // in the channel. The roster diff matches by NAME, so if no resolution
    // observes the roster while the name is absent -- a fast rejoin between
    // two events -- present_here and was_present are BOTH true, no departure
    // fires, no re-invite is ever issued, and members_present counts a dead
    // id for the rest of the meeting. The director keys, is told "1 of 1
    // present", and that person hears nothing.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);   // all-talent
        respond(tb, chan_id(2).c_str(), kOk);   // Sarah private

        svc.participants.users.push_back(make_user(8001, "Sarah"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 2, "setup: Sarah's join did not invite her");
        tb_join_response(tb, chan_id(1).c_str(), 8001, kOk);
        tb_join_response(tb, chan_id(2).c_str(), 8001, kOk);
        std::size_t present = 0, total = 0;
        tb.members_present_for_target("Sarah", &present, &total);
        check(present == 1 && total == 1, "setup: Sarah was not counted as present");

        // THE WINDOW: she leaves and rejoins under a new id with no
        // resolution in between, so the very next snapshot shows the SAME
        // NAME under a DIFFERENT uid.
        svc.participants.users.clear();
        svc.participants.users.push_back(make_user(9001, "Sarah"));
        resolve(tb, host);

        check(svc.ctrl.invited.size() == 4,
              "M1: A REJOIN UNDER A NEW USER ID WAS NEVER RE-INVITED -- the "
              "name matched, so no departure was detected, and the stale entry "
              "keeps claiming presence for an id that is not in the meeting");
        check(svc.ctrl.invited.back().second == 9001,
              "M1: the re-invite went to the OLD user id");
        tb.members_present_for_target("Sarah", &present, &total);
        check(present == 0 && total == 1,
              "M1: the stale entry was still counted as present between the "
              "prune and the new id's own join response -- \"1 of 1 present\" "
              "for someone who hears nothing is the whole finding");

        tb_join_response(tb, chan_id(1).c_str(), 9001, kOk);
        tb_join_response(tb, chan_id(2).c_str(), 9001, kOk);
        tb.members_present_for_target("Sarah", &present, &total);
        check(present == 1 && total == 1,
              "M1: the re-invited id's own join response did not restore the "
              "presence count");

        // Idempotent afterwards: the healed state must not itself become a
        // source of repeated invites.
        resolve(tb, host);
        check(svc.ctrl.invited.size() == 4, "M1: the healed state re-invited");
    }

    // ...and the departure edge must survive a resolution that REFUSES.
    // has_pending_work() used to drop the whole resolution -- comment and
    // all: "a refusal here costs nothing but a delay ... the next roster
    // event gets another chance". True for invites, false for departures: a
    // talkback probe runs up to ~30s, and the next roster event compares
    // against the roster as it is THEN, so the edge is not delayed, it is
    // destroyed.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        svc.participants.users.push_back(make_user(8001, "Sarah"));
        resolve(tb, host);
        tb_join_response(tb, chan_id(1).c_str(), 8001, kOk);
        tb_join_response(tb, chan_id(2).c_str(), 8001, kOk);
        const std::size_t invited_before = svc.ctrl.invited.size();

        // A probe claims the arbiter: has_pending_work() is now true.
        check(tb.probe(&host, "Someone"), "setup: the probe refused to start");

        svc.participants.users.clear();
        svc.participants.users.push_back(make_user(9001, "Sarah"));
        resolve(tb, host);

        std::size_t present = 0, total = 0;
        tb.members_present_for_target("Sarah", &present, &total);
        check(present == 0,
              "M1: A DEPARTURE THAT LANDED INSIDE A PROBE WAS DROPPED "
              "ENTIRELY -- the stale id claims presence for the rest of the "
              "meeting, and no later roster event can rediscover the edge");
        check(svc.ctrl.invited.size() == invited_before,
              "M1: the invite half was NOT gated on the probe -- that is the "
              "half the gate exists for (Begin/Add/Execute interleaving with "
              "the probe's driving thread)");
    }

    // -- LIVE PRODUCTION 2026-08-29: MEMBERSHIP MUST BE ACOUSTICALLY NEUTRAL
    // UNTIL KEYED ---------------------------------------------------------
    //
    // Talent reported losing meeting audio the moment they were ASSIGNED to a
    // talkback channel, before any key was pressed. Nothing in the engine
    // ducks at provision, so Zoom's own default for a channel member is
    // ducked -- and a pre-provisioned architecture, whose whole premise is
    // that channels can stand for the length of the show, turned that into a
    // standing duck. The engine now sets the level explicitly at creation
    // instead of inheriting whatever Zoom chose.
    //
    // This block is the pin. Delete the SetChannelBackgroundVolume call in
    // onCreateChannelResponse's nomination branch and every check below fails.
    {
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        FakeMeetingService svc;

        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(4101, "Sarah"));
        svc.participants.users.push_back(make_user(4102, "Luis"));
        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a two-name plan");
        // 1 all-talent + 2 private = 3 channels.
        for (int i = 1; i <= 3; ++i)
            respond(tb, chan_id(i).c_str(), kOk);
        check(svc.ctrl.creates == 3, "setup: the two-name plan did not provision 3 channels");

        // (1) EXACTLY ONE SET PER PROVISIONED CHANNEL, and it is neutral.
        check(svc.ctrl.volumes.size() == 3,
              "provisioning did not set background volume exactly once per channel");
        check(count_volume_sets(svc.ctrl.volumes, 1.0f) == 3,
              "PROVISIONING LEFT ZOOM'S DEFAULT IN PLACE -- this is the live "
              "2026-08-29 defect: talent ducked by being assigned, no key pressed");
        check(volume_channels_distinct(svc.ctrl.volumes),
              "one channel was set twice while another was never set at all");

        // (2) NEUTRAL BEFORE ANYBODY IS INVITED. A member invited into a
        // channel still at Zoom's default hears the duck for however long the
        // gap lasts, which is the defect in miniature.
        check(svc.ctrl.first_invite_call >= 0, "setup: nobody was invited");
        check(svc.ctrl.first_volume_call >= 0 &&
              svc.ctrl.first_volume_call < svc.ctrl.first_invite_call,
              "a member was invited into a channel BEFORE its background volume "
              "was made neutral");

        // (3) ONE REPORT LINE PER CHANNEL, not per member. Six members are
        // invited across these three channels; per-member reporting would be
        // six lines, and a 13-channel show's worth of that is the message-storm
        // shape this codebase already has a live incident about.
        int neutral_reports = 0;
        for (const auto &l : lines)
            if (line_has(l, "\"cmd\":\"talkback_nominate\"") &&
                line_has(l, "\"stage\":\"background_volume_neutral\""))
                ++neutral_reports;
        check(neutral_reports == 3,
              "the neutral background-volume set was not reported once per "
              "channel");

        // (4) THE SETTING IS CHANNEL-SCOPED, SO CREATION-TIME IS ENOUGH.
        // SetChannelBackgroundVolume is keyed by channelID alone -- there is no
        // per-member variant in the controller at all -- so a member invited
        // LATER by a roster re-resolution inherits the channel's value. Prove
        // the engine agrees: a leave+rejoin under a new id re-invites (the
        // roster path really did run) and issues NO further volume calls.
        const std::size_t invited_before = svc.ctrl.invited.size();
        svc.participants.users.clear();
        svc.participants.users.push_back(make_user(4201, "Sarah"));
        svc.participants.users.push_back(make_user(4202, "Luis"));
        resolve(tb, host);
        check(svc.ctrl.invited.size() > invited_before,
              "setup: the rejoin never re-invited, so this proves nothing about "
              "what a late member join does to volume");
        check(svc.ctrl.volumes.size() == 3,
              "a LATE member join re-asserted background volume -- the setting is "
              "a CHANNEL property (no user parameter exists), so one set at "
              "creation is the whole contract");

        EngineIpc::test_sink() = nullptr;
    }

    // ═══ TALKBACK DELIVERY LAW 1: THE MIC HAS TO BE OPEN ════════════════════
    //
    // ZComms, 2026-08-29, live: talkback delivers ONLY while this client's own
    // meeting audio is unmuted. Muted, SendAudioDataToChannel is ACCEPTED --
    // success codes, members confirmed, zero failures -- and every member
    // hears silence. The operator's own production that day had the bot muted
    // by the host, so every send would have been that ghost.
    //
    // (a) A key on a MUTED client unmutes it, and does so BEFORE the first
    // buffer reaches Zoom. Ordering is the fact, not the count: an unmute
    // AFTER the first send is a director's first syllable into the void, which
    // is the failure mode the whole talkback feature is written against.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        // The bot is in the meeting and the HOST HAS MUTED IT -- the live
        // 2026-08-29 state.
        svc.participants.has_self = true;
        svc.participants.self = make_user(7700, "CoreVideo Engine");
        svc.participants.self.muted = true;
        svc.participants.users.push_back(make_user(7001, "Sarah"));

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);

        ShmRegion region{};
        const std::string region_name = "ZoomObsPluginTest_talkback_mic_open";
        check(shm_region_create(region, region_name,
                                shm_audio_region_bytes(kTalkbackSlotBytes)),
              "the test could not create a talkback ring region");
        talkback_ring_init(static_cast<ShmAudioHeader *>(region.ptr), 48000, 1);
        check(tb.open_audio(region_name, 48000, 1),
              "the engine refused to open the test's talkback ring");

        check(tb.session_start(&host, "Sarah"), "keying Sarah's channel was refused");
        check(svc.audio.unmuted.size() == 1 && svc.audio.unmuted[0] == 7700,
              "THE KEY WENT LIVE OVER A MUTED BOT -- talkback delivers only "
              "while this client's own meeting audio is open, and every send "
              "from here would have been ACCEPTED AND SILENT");

        int16_t pcm[480] = {0};
        for (std::size_t i = 0; i < 480; ++i) pcm[i] = static_cast<int16_t>(i);
        check(talkback_ring_publish(region.ptr, pcm, sizeof(pcm), 1),
              "the test could not publish a buffer into the ring");
        tb.drain_audio();
        check(!svc.ctrl.sends.empty(), "setup: nothing was sent, so ordering "
              "proves nothing");
        check(svc.audio.first_unmute_call >= 0 &&
                  svc.audio.first_unmute_call < svc.ctrl.first_send_call,
              "THE MIC WAS OPENED AFTER THE FIRST BUFFER WENT TO ZOOM -- every "
              "send before that instant is accepted and inaudible");

        // The session report has to SAY the mic is open, on the line the
        // plugin's state machine consumes, or the banner can never tell "on
        // air" from "on air but muted by the host".
        bool live_says_open = false;
        for (const auto &l : lines)
            if (line_has(l, "\"cmd\":\"talkback_session\"") &&
                line_has(l, "\"live\":true") && line_has(l, "\"mic\":\"open\""))
                live_says_open = true;
        check(live_says_open,
              "the live session report did not carry \"mic\":\"open\"");

        // (b) THE RE-ASSERT. A host can mute the bot mid-key -- so "open at
        // session_start()" is not a state that stays true, and past the moment
        // it stops being true every buffer is the accepted-but-silent ghost
        // again. The pump rides main.cpp's command-loop idle turn beside
        // nomination_tick(); here the 2s deadline is expired the same way
        // every other deadline in this file is.
        svc.participants.self.muted = true;   // the host re-mutes the bot
        tb.mic_tick();                        // ...but the interval has not passed
        check(svc.audio.unmuted.size() == 1,
              "the mic re-assert ran before its interval -- it is a 2s pump, "
              "not a per-turn SDK call");
        tb.debug_expire_mic_assert_for_test();
        tb.mic_tick();
        check(svc.audio.unmuted.size() == 2,
              "A HOST RE-MUTED THE BOT MID-KEY AND NOTHING RE-OPENED IT -- the "
              "director stays on air, believing they are heard, sending "
              "accepted silence for the rest of the press");

        // (c) THE RESTORE. The bot was muted before the key; it goes back to
        // muted after it. A bot left hot on a machine running a live
        // production is the worse failure, and it is not ours to leave.
        tb.session_stop();
        check(svc.audio.muted.size() == 1 && svc.audio.muted[0] == 7700,
              "A BOT THE HOST HAD MUTED WAS LEFT UNMUTED AFTER THE KEY CLOSED");

        // ...and the pump stops with the key. Nothing to re-assert between
        // presses: the mic is opened because audio is about to flow.
        const std::size_t unmutes_after_stop = svc.audio.unmuted.size();
        tb.debug_expire_mic_assert_for_test();
        tb.mic_tick();
        check(svc.audio.unmuted.size() == unmutes_after_stop,
              "the mic re-assert kept unmuting after the key closed -- it "
              "would fight the host over a bot that is talking to nobody");

        EngineIpc::test_sink() = nullptr;
        shm_region_destroy(region);
    }

    // (d) A CLIENT THAT WAS ALREADY OPEN is not touched, and is not re-muted
    // on release. "Restore the prior state" means exactly that; muting a bot
    // the operator deliberately left unmuted would be this file inventing
    // state it was never given.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.has_self = true;
        svc.participants.self = make_user(7700, "CoreVideo Engine");
        svc.participants.self.muted = false;   // already open
        svc.participants.users.push_back(make_user(7001, "Sarah"));

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);
        check(tb.session_start(&host, "Sarah"), "keying Sarah's channel was refused");
        check(svc.audio.unmuted.empty(),
              "an already-open mic was unmuted anyway -- an SDK call on the key "
              "path that changes nothing is exactly what the key path may not "
              "carry");
        tb.session_stop();
        check(svc.audio.muted.empty(),
              "A CLIENT THAT WAS ALREADY UNMUTED WAS MUTED ON KEY RELEASE -- "
              "the restore puts back the PRIOR state, it does not impose one");
    }

    // (e) A MEETING THAT LOCKS MUTE. Some do, and there is no fix on this
    // side. The key stays LIVE -- the channels are real, the audio path is
    // real, and the instant a host unmutes the bot the next buffer is heard,
    // so refusing would take away the operator's last move mid-show -- but the
    // session report must NOT say plain "live", or the accepted-but-silent
    // ghost is invisible all over again. "ON AIR -- but the bot is muted by
    // the host" is a sentence the banner can only say if this field exists.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.participants.has_self = true;
        svc.participants.self = make_user(7700, "CoreVideo Engine");
        svc.participants.self.muted = true;
        svc.audio.unmute_result = ZOOMSDK::SDKERR_NO_PERMISSION;
        svc.participants.users.push_back(make_user(7001, "Sarah"));

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);
        check(tb.session_start(&host, "Sarah"),
              "a mic the meeting refuses to unmute must not REFUSE the key -- "
              "the channels are live and a host can still unmute the bot");
        check(tb.session_live(), "the key did not report live");

        bool live_says_blocked = false;
        for (const auto &l : lines)
            if (line_has(l, "\"cmd\":\"talkback_session\"") &&
                line_has(l, "\"live\":true") && line_has(l, "\"mic\":\"blocked\""))
                live_says_blocked = true;
        check(live_says_blocked,
              "A KEY OVER A MIC THAT COULD NOT BE OPENED REPORTED PLAIN "
              "\"live\" -- that one word is what made the accepted-but-silent "
              "ghost invisible in the first place");

        // A failed unmute changed nothing, so there is nothing to put back.
        tb.session_stop();
        check(svc.audio.muted.empty(),
              "a key whose unmute FAILED still re-muted on release -- it never "
              "opened anything, so it has nothing to restore");

        EngineIpc::test_sink() = nullptr;
    }

    // (f) TASK 2B (2026-09-05): THE SEAM ITSELF FAILING, WITH NO SDK OBJECT
    // INVOLVED AT ALL. Test (e) above drives FakeAudioController::unmute_result
    // -- a genuine Zoom-side refusal, reaching ensure_mic_open() through
    // TalkbackHost::set_self_muted() returning TalkbackResult::Unknown after
    // actually calling UnMuteAudio(). This one drives
    // FakeTalkbackHost::fail_set_self_muted, which returns
    // TalkbackResult::Unknown WITHOUT touching FakeAudioController at all --
    // the seam-level failure shape (e.g. a concrete adapter that could not
    // even resolve a controller to call). The invariant this task's brief
    // names is that ensure_mic_open() must treat ANY non-Ok TalkbackResult
    // from set_self_muted() the same way, regardless of why the seam
    // returned it: m_mic_open stays false, the key stays LIVE (Law 1 never
    // refuses the key over a mic it cannot open), and the live line reports
    // "mic":"blocked" -- never "open".
    //
    // MUTATION-PROVEN (see task-2b-report.md for the run): changing
    // ensure_mic_open()'s `m_mic_open = (r == TalkbackResult::Ok);` to an
    // unconditional `m_mic_open = true;` makes this test's "mic":"blocked"/
    // "mic":"open" assertions fail (traced by hand, not executed by ctest --
    // this file is Windows-gated and this machine has no Windows toolchain;
    // see the report for what WAS executed).
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.participants.has_self = true;
        svc.participants.self = make_user(7700, "CoreVideo Engine");
        svc.participants.self.muted = true;
        host.fail_set_self_muted = true;
        svc.participants.users.push_back(make_user(7001, "Sarah"));

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);
        check(tb.session_start(&host, "Sarah"),
              "a mic the SEAM itself could not open must not refuse the key "
              "-- the channels are live and a host can still unmute the bot "
              "by hand");
        check(tb.session_live(), "the key did not report live");
        check(svc.audio.unmuted.empty(),
              "fail_set_self_muted was set, so no real UnMuteAudio call "
              "should have reached the fake audio controller at all");

        bool live_says_blocked = false;
        bool live_says_open = false;
        for (const auto &l : lines) {
            if (!line_has(l, "\"cmd\":\"talkback_session\"") ||
                !line_has(l, "\"live\":true"))
                continue;
            if (line_has(l, "\"mic\":\"blocked\"")) live_says_blocked = true;
            if (line_has(l, "\"mic\":\"open\"")) live_says_open = true;
        }
        check(live_says_blocked,
              "A SEAM-LEVEL set_self_muted() FAILURE DID NOT REPORT "
              "\"mic\":\"blocked\" -- m_mic_open must stay false when the seam "
              "cannot open the mic, exactly as when the SDK itself refuses");
        check(!live_says_open,
              "a failed set_self_muted() was read as an open mic -- the exact "
              "fail-open shape this task's brief warns against");

        tb.session_stop();
        check(svc.audio.muted.empty(),
              "a key whose seam-level unmute FAILED still re-muted on "
              "release -- it never opened anything, so it has nothing to "
              "restore");

        EngineIpc::test_sink() = nullptr;
    }

    // ═══ TALKBACK DELIVERY LAW 2: ONE MEMBERSHIP CALL PER ~600ms ════════════
    //
    // ZComms, 2026-08-29, live 12-person meeting: Zoom's rate limit is per
    // membership CALL and INVITES COUNT -- their healer drew
    // SDKERR_TOO_FREQUENT_CALL on every pass while making no creates at all.
    // Our ladder paced creates (2026-08-26) and fired invites UNPACED, in
    // bursts: every member of a channel back to back inside
    // onCreateChannelResponse, and every re-resolved name at once from
    // resolve_roster_change(). Two channels passed the 2026-08-26 gate because
    // two channels is two creates and two invites; 13 channels and 24 invites
    // is the case that trips it.
    //
    // (a) ONE call per turn, whatever its kind, and none before the deadline.
    // This block deliberately does not use respond()/resolve(): the fact being
    // pinned is precisely what does NOT happen in between.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(8001, "Sarah"));
        svc.participants.users.push_back(make_user(8002, "Luis"));

        // Two nominees plan 3 channels: all-talent (both) + one private each.
        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a two-name plan");
        check(svc.ctrl.creates == 1, "setup: the first create was not issued");

        // Channel 1 is the all-talent slice, so its response queues TWO
        // invites -- the exact back-to-back burst Zoom refuses.
        tb_create_response(tb, chan_id(1).c_str(), kOk);
        check(svc.ctrl.invited.empty(),
              "THE UNPACED BURST: the provisioning branch issued its invites "
              "inline, back to back, from inside onCreateChannelResponse -- "
              "which is the shape ZComms measured Zoom refusing with code 18");

        tb.nomination_tick();
        check(svc.ctrl.creates == 1 && svc.ctrl.invited.empty(),
              "the pump issued a membership call before the shared deadline");

        // Turn 1: the create. Creates take priority within a turn -- a channel
        // that does not exist cannot be invited into.
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.creates == 2 && svc.ctrl.invited.empty(),
              "A CREATE AND AN INVITE WENT OUT IN THE SAME TURN -- creates and "
              "invites share ONE 600ms budget, because Zoom's limit counts "
              "calls and not call kinds");

        // Turn 2: the first invite -- and only after its own deadline.
        tb.nomination_tick();
        check(svc.ctrl.invited.empty(),
              "the invite pump ignored the shared deadline the create had just "
              "stamped");
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.invited.size() == 1,
              "the pump never issued the queued invite once its deadline "
              "passed -- the burst was suppressed and nothing replaced it");

        // Turn 3: the second invite, not before.
        tb.nomination_tick();
        check(svc.ctrl.invited.size() == 1,
              "TWO INVITES WENT OUT IN ONE TURN -- the pacer counts calls, and "
              "an all-talent channel's ten members are ten of them");
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.invited.size() == 2, "the second queued invite never issued");

        // ROUND-ROBIN, per ZComms's law. Channel 2 (Sarah's private) responds
        // and queues one invite while channel 1 still has one outstanding of
        // its own; the next invite must go to the OTHER channel, not to
        // whatever is at the front of a FIFO. At one call per 600ms, FIFO
        // order is what decides whether the last talent's own channel is
        // confirmed at second 2 or second 20 -- and it is the private channels
        // the director keys.
        //
        // Rebuilt from scratch so the queue state is exactly the two-channel
        // interleave this asserts on.
    }
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(8001, "Sarah"));
        svc.participants.users.push_back(make_user(8002, "Luis"));
        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a two-name plan");

        tb_create_response(tb, chan_id(1).c_str(), kOk);   // all-talent: 2 invites queued
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // create 2
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // invite 1, channel 1
        check(svc.ctrl.invited.size() == 1 &&
                  svc.ctrl.invited[0].first == utf8_of(chan_id(1).c_str()),
              "setup: the first invite did not go to the all-talent channel");

        tb_create_response(tb, chan_id(2).c_str(), kOk);   // a private: 1 invite queued
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // create 3
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // invite 2 -- which channel?
        check(svc.ctrl.invited.size() == 2 &&
                  svc.ctrl.invited[1].first == utf8_of(chan_id(2).c_str()),
              "THE PACER IS NOT ROUND-ROBIN -- it drained the all-talent "
              "channel's queue before touching the private channel that had "
              "been waiting, which at one call per 600ms is how the last "
              "talent's own key ends up unconfirmed for twenty seconds");
    }

    // (a2) THE FLOOR IS SHARED IN THE DIRECTION THAT ACTUALLY BITES: a create
    // that is due MUST NOT go out on the heels of an invite that just did.
    //
    // FOUND BY MUTATION, NOT BY REVIEW -- and it is the whole point of Law 2.
    // Deleting the shared floor from nomination_tick() left every check above
    // green, because they all reach the "next turn" state through
    // debug_expire_create_spacing_for_test(), which expires the floor along
    // with the create's own deadline. A guard whose only test also disables
    // the thing it guards against asserts nothing; this file's own history has
    // that shape three times now.
    //
    // debug_expire_create_schedule_for_test() is the narrow hook that can
    // express the missing state: the create is due, the floor is not. Without
    // the shared floor the create leaves ~0ms after the invite -- exactly the
    // back-to-back membership pair Zoom answers with SDKERR_TOO_FREQUENT_CALL,
    // and exactly what the 2026-08-26 create-only pacing could not prevent
    // once invites became the other half of the same budget.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(8201, "Sarah"));
        svc.participants.users.push_back(make_user(8202, "Luis"));
        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a two-name plan");

        tb_create_response(tb, chan_id(1).c_str(), kOk);   // 2 invites queued
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // turn: create 2
        check(svc.ctrl.creates == 2, "setup: the second create never issued");
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // turn: invite 1
        check(svc.ctrl.invited.size() == 1, "setup: the first invite never issued");

        // Channel 2 answers, so a THIRD create is now scheduled -- and an
        // invite went out a moment ago, so the shared floor is fresh.
        tb_create_response(tb, chan_id(2).c_str(), kOk);
        const int creates_before = svc.ctrl.creates;

        // The create's OWN deadline passes. The floor has not.
        tb.debug_expire_create_schedule_for_test();
        tb.nomination_tick();
        check(svc.ctrl.creates == creates_before,
              "A CREATE WENT OUT ON THE HEELS OF AN INVITE -- creates and "
              "invites share ONE budget because Zoom's rate limit counts "
              "membership CALLS, not call kinds (ZComms, 2026-08-29); pacing "
              "only the creates against each other is the 2026-08-26 fix, and "
              "it is not enough");

        // ...and it is a WAIT, not a stall: once the floor passes too, the
        // create issues. Without this half, "creates == creates_before" above
        // would also be true of a pacer that had simply wedged.
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.creates == creates_before + 1,
              "the create never issued once the shared floor passed -- the "
              "ladder is stalled, not paced");
    }

    // (b) AN INVITE REFUSED CODE 18 IS A WAIT, NOT A FAILURE -- the same
    // ruling the create ladder has lived under since 2026-08-26, applied to
    // the call kind ZComms proved it also applies to. Before this, an invite
    // refused 18 reported its line and stopped: nothing marked the name
    // present or failed, so nothing would ever retry it except a future roster
    // event for a person who is already in the meeting and staying put. Three
    // people in the 2026-08-29 show were refused code 18 on the all-talent
    // invite and were only admitted because an unrelated second invite for
    // their private channel happened a second later.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.participants.users.push_back(make_user(8101, "Sarah"));
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");

        // Zoom refuses exactly the next invite.
        svc.ctrl.invite_rate_limit_next = 1;
        respond(tb, chan_id(1).c_str(), kOk);
        check(svc.ctrl.invite_rate_limited == 1,
              "setup: the fake never refused an invite, so this proves nothing "
              "about the invite rate limit");
        check(svc.ctrl.invited.size() == 1,
              "A RATE-LIMITED INVITE WAS NEVER RETRIED -- the talent has a "
              "channel, a completed nomination and no membership, and the dock "
              "shows them as ready to key");

        int retry_lines = 0, gaveup_lines = 0;
        for (const auto &l : lines) {
            if (line_has(l, "\"stage\":\"invite_rate_limited_retry\"")) ++retry_lines;
            if (line_has(l, "\"stage\":\"invite_rate_limited\"")) ++gaveup_lines;
        }
        check(retry_lines == 1,
              "the rate-limited invite did not report its backoff -- \"waiting, "
              "will retry\" and \"gave up\" must not read the same in the log");
        check(gaveup_lines == 0,
              "an invite that succeeded on its retry also reported giving up");

        // The ladder itself is untouched: one person's membership is not the
        // nomination's progress, and an invite 18 must never destroy channels.
        check(count_abort_reports(lines) == 0,
              "A RATE-LIMITED INVITE ABORTED THE NOMINATION LADDER -- the "
              "channels are fine; one membership call was told to wait");

        EngineIpc::test_sink() = nullptr;
    }

    // ═══ REVIEW ROUND 1 ═════════════════════════════════════════════════════

    // M1b: A MID-KEY HOST MUTE RE-REPORTS THE SESSION STATE, on the edge.
    //
    // mic_tick() used to report only its own "mic_open" STAGE line, and
    // report_session_state() was never called again after session_start(). So
    // a host re-muting the bot at second 30 of a latched key, in a meeting
    // that then refuses our unmute, left the plugin's stored `mic` at "open"
    // from the opening press for the rest of the key: the banner went on
    // saying a clean ON AIR while nothing was audible. That is the
    // accepted-but-silent ghost surviving the law written to expose it.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };

        svc.participants.has_self = true;
        svc.participants.self = make_user(7700, "CoreVideo Engine");
        svc.participants.self.muted = false;     // opens clean
        svc.participants.users.push_back(make_user(7001, "Sarah"));

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        respond(tb, chan_id(1).c_str(), kOk);
        respond(tb, chan_id(2).c_str(), kOk);
        check(tb.session_start(&host, "Sarah"), "keying Sarah's channel was refused");

        auto count_state_lines = [&](const char *mic) {
            int n = 0;
            for (const auto &l : lines)
                if (line_has(l, "\"cmd\":\"talkback_session\"") &&
                    line_has(l, "\"live\":true") &&
                    line_has(l, std::string("\"mic\":\"") + mic + "\""))
                    ++n;
            return n;
        };
        check(count_state_lines("open") == 1,
              "setup: the opening press did not report a live/open session");

        // The host mutes the bot mid-key AND the meeting refuses our unmute.
        svc.participants.self.muted = true;
        svc.audio.unmute_result = ZOOMSDK::SDKERR_NO_PERMISSION;
        tb.debug_expire_mic_assert_for_test();
        tb.mic_tick();
        check(count_state_lines("blocked") == 1,
              "A HOST MUTED THE BOT MID-KEY AND THE SESSION STATE NEVER MOVED "
              "-- the plugin keeps the \"open\" it stored at the press, the "
              "banner keeps saying a clean ON AIR, and every buffer from here "
              "is accepted and inaudible");

        // EDGE ONLY. A latched key re-asserts every 2s for as long as it is
        // held; a line per assertion is 30 a minute saying nothing changed,
        // which is the message-storm shape this codebase has a live incident
        // about -- and the plugin's handler takes a mutex per line.
        for (int i = 0; i < 5; ++i) {
            tb.debug_expire_mic_assert_for_test();
            tb.mic_tick();
        }
        check(count_state_lines("blocked") == 1,
              "the re-assert re-reported an UNCHANGED state -- it must fire on "
              "the edge, not on every 2s tick");

        // ...and it clears when the host relents, or the alarm is permanent
        // and the operator learns to ignore it.
        svc.audio.unmute_result = ZOOMSDK::SDKERR_SUCCESS;
        tb.debug_expire_mic_assert_for_test();
        tb.mic_tick();
        check(count_state_lines("open") == 2,
              "the mic re-opening mid-key never reported the recovery -- a "
              "banner stuck on an alarm that has cleared is the same defect "
              "pointing the other way");

        EngineIpc::test_sink() = nullptr;
        tb.session_stop();
    }

    // M2: THE INVITE PUMP MUST NOT BATCH WHILE THE PROBE'S DRIVING THREAD MAY.
    //
    // Begin/Add/ExecuteBatchInviteUsers is a FOURTH Begin/Add/Execute sequence
    // (tick()'s inventory counted three) and the first on the command loop
    // that does not sit inside an `owner == Nomination` branch -- so fact 2 of
    // that inventory's chain, which excludes all the others, does not cover
    // it. Law 2 is why: invites used to be issued inline from
    // onCreateChannelResponse (where fact 2 did cover them) and are now issued
    // from a free-running idle pump up to ~22s later.
    //
    // THE REACHABLE SEQUENCE, and it is the natural operator flow: a big plan
    // reports nominate_done as soon as the LAST CREATE responds, with invites
    // still queued; the arbiter is free and no session is live, so the
    // operator pressing Talkback probe passes every gate probe() has and
    // main.cpp spawns the driving thread. From then until the probe settles,
    // this pump would batch against a thread that may be mid batch-destroy.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(8301, "Sarah"));
        svc.participants.users.push_back(make_user(8302, "Luis"));

        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a two-name plan");

        // DRIVE THE LADDER TO ITS TERMINAL WITH THE INVITES STILL QUEUED --
        // which is the whole trigger, and is what nominate_done means: it
        // fires on the LAST CREATE's response, not on the last invite's. Every
        // tick below issues a create (creates take priority within a turn), so
        // the queue only grows.
        //
        // NOT respond(): that helper drains the pacer to quiescence, which is
        // exactly the state this test must NOT be in.
        tb_create_response(tb, chan_id(1).c_str(), kOk);   // all-talent: +2
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // create 2
        tb_create_response(tb, chan_id(2).c_str(), kOk);   // Sarah private: +1
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // create 3
        tb_create_response(tb, chan_id(3).c_str(), kOk);   // Luis private: +1
        check(svc.ctrl.creates == 3, "setup: the plan did not provision 3 channels");
        check(svc.ctrl.invited.empty(),
              "setup: an invite escaped, so the queue this test needs is short");

        // The arbiter is free, no create is scheduled, no session is live --
        // so the operator pressing Talkback probe in the dock passes every
        // gate probe() has, and main.cpp spawns the driving thread on this
        // `true`. From here until the probe settles, the driving thread may be
        // inside drain_stray_channels() or the Destroying phase's own
        // batch-destroy at any moment.
        check(tb.probe(&host, "Sarah"),
              "setup: the probe refused to start, so this proves nothing about "
              "the exclusion -- and the refusal itself would be the M2 trigger "
              "disappearing, not the hazard");
        check(tb.has_pending_work(),
              "setup: the probe left no pending work, so there is no driving "
              "thread to be excluded from");

        for (int i = 0; i < 20; ++i) {
            tb.debug_expire_create_spacing_for_test();
            tb.nomination_tick();
        }
        check(svc.ctrl.invited.empty(),
              "THE INVITE PUMP BATCHED WHILE THE PROBE'S DRIVING THREAD WAS "
              "ALIVE -- Begin/Add/ExecuteBatchInviteUsers from the 50ms idle "
              "hook against a thread that may be mid batch-destroy on the same "
              "controller. By this file's own hazard model that merges or "
              "corrupts batches: an invite landing in a destroy batch, or a "
              "destroy taking a channel it was not given, which is a "
              "provisioned channel torn down mid-show");

        // DEFERRED, NOT DROPPED -- and that is what makes the gate free. The
        // queue is untouched, so the moment the probe settles the same invites
        // go out. A gate that dropped them would trade a batch hazard for
        // silently unconfirmed talent, which is the worse of the two.
        tb_create_response(tb, 
            chan_id(9).c_str(),
            IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION);
        check(!tb.has_pending_work(),
              "setup: the probe did not settle, so the deferral half below "
              "would pass for the wrong reason");
        for (int i = 0; i < 20; ++i) {
            tb.debug_expire_create_spacing_for_test();
            tb.nomination_tick();
        }
        check(svc.ctrl.invited.size() == 4,
              "THE QUEUED INVITES WERE LOST, not deferred -- the talent have "
              "channels and no membership, and nothing will ever retry them");
    }

    // m2 (mutation-proved by the reviewer): A QUEUED INVITE MUST NEVER OUTLIVE
    // ITS CHANNEL.
    //
    // The header calls this a STRUCTURAL guarantee -- "not a state that exists
    // rather than one argued to be unreachable" -- and removing
    // m_invite_queue.clear() from nomination_destroy_provisioned() left 68/68
    // green. It is load-bearing: without it, a ladder abort or a re-nomination
    // leaves the queue naming destroyed channel ids and the pump issues
    // membership calls into channels Zoom no longer has, spending pacer turns
    // the LIVE channels' invites need.
    //
    // Driven through a code-18 REQUEUED invite specifically, because that is
    // the entry most likely to still be sitting in the queue when a ladder
    // dies: it has a backoff deadline in the future and nothing else to do.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(8401, "Sarah"));
        svc.participants.users.push_back(make_user(8402, "Luis"));

        check(tb.nominate(&host, {"Sarah", "Luis"}), "nominate refused a two-name plan");
        tb_create_response(tb, chan_id(1).c_str(), kOk);   // 2 invites queued

        // Zoom refuses every invite, so both stay queued behind a backoff.
        svc.ctrl.invite_rate_limit_next = 2;
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // create 2
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // invite -> 18
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // invite -> 18
        check(svc.ctrl.invite_rate_limited == 2,
              "setup: the fake never refused the invites, so nothing is queued "
              "behind a backoff and this proves nothing");
        check(svc.ctrl.invited.empty(), "setup: a refused invite was recorded");

        // The ladder dies and takes its channels with it.
        const std::size_t destroyed_before = svc.ctrl.destroyed.size();
        tb_create_response(tb, chan_id(2).c_str(),
                                   IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION);
        check(svc.ctrl.destroyed.size() > destroyed_before,
              "setup: the failing ladder did not tear its channels down");

        // Every turn the pacer will ever have. Nothing may be invited: those
        // channel ids do not exist any more.
        for (int i = 0; i < 40; ++i) {
            tb.debug_expire_create_spacing_for_test();
            tb.nomination_tick();
        }
        check(svc.ctrl.invited.empty(),
              "A QUEUED INVITE OUTLIVED ITS CHANNEL -- the ladder aborted and "
              "destroyed these ids, and the pump then issued membership calls "
              "into channels Zoom no longer has, spending the pacer's turns on "
              "them");
    }

    // m1 (mutation-proved by the reviewer): AN INVITE'S CODE-18 BACKOFF IS
    // ACTUALLY HELD.
    //
    // "18 is a wait, not a failure" has two halves: requeue (pinned above) and
    // DO NOT RETRY INSTANTLY. The second was unpinnable, because the only hook
    // that advanced the pacer -- debug_expire_create_spacing_for_test() --
    // also expired every queued invite's own backoff, and every invite
    // assertion in this file goes through it 128 times per call. Deleting the
    // per-entry not_before check left 68/68 green.
    //
    // debug_expire_membership_floor_for_test() is the narrow hook that can
    // express the one state this rule is about: the pacer is OPEN and this
    // invite is still backed off. Exactly the shape this commit already fixed
    // for the create-side floor, one field over, in the same change.
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.participants.users.push_back(make_user(8501, "Sarah"));

        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        tb_create_response(tb, chan_id(1).c_str(), kOk);   // 1 invite queued

        svc.ctrl.invite_rate_limit_next = 1;
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // create 2
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();                                   // invite -> 18
        check(svc.ctrl.invite_rate_limited == 1,
              "setup: the fake never refused the invite");
        check(svc.ctrl.invited.empty(), "setup: a refused invite was recorded");

        // Open the PACER, and only the pacer. The invite's own backoff stands.
        for (int i = 0; i < 10; ++i) {
            tb.debug_expire_membership_floor_for_test();
            tb.nomination_tick();
        }
        check(svc.ctrl.invited.empty(),
              "A RATE-LIMITED INVITE WAS RETRIED INSTANTLY -- Zoom said \"not "
              "yet\" and the pump asked again on the very next turn, which is "
              "the burst that drew the 18 in the first place");

        // ...and it is a WAIT, not a wedge: once the backoff itself passes,
        // the invite goes. Without this half the check above is also true of a
        // queue that simply stopped.
        tb.debug_expire_create_spacing_for_test();
        tb.nomination_tick();
        check(svc.ctrl.invited.size() == 1,
              "the backed-off invite never issued once its own deadline passed "
              "-- that is a stalled queue, not a paced one");
    }

    // -- Task 1 (2026-09-04): the TalkbackSdk seam's result normalisation
    // reaches the ladder's own retry decision, not just the fake's own
    // pass-through -----------------------------------------------------------
    //
    // Law 2's signal must survive the seam and reach the LADDER's retry
    // decision. Asserting the fake returns what it was told is a tautology;
    // what matters is that a too-frequent create is retried rather than
    // reported as a terminal failure, which is the behaviour the whole backoff
    // exists for.
    //
    // Two adjustments from task-1-brief.md's Step 2 sketch, both because the
    // sketch's exact code does not compile/pass against the real engine (see
    // task-1-report.md for why):
    //   (1) `EngineTalkback::nominate_for_test()` does not exist and has no
    //       natural equivalent -- nominate() is the ladder's one entry point
    //       and needs an IMeetingService to gate against, even though this
    //       test's whole point is to bypass the SDK via the seam. Driven with
    //       the existing public nominate() plus a FakeMeetingService, exactly
    //       as every other test in this file already is.
    //   (2) `debug_expire_membership_floor_for_test()` alone only opens the
    //       shared membership-call floor (m_membership_next_at); it never
    //       touches the CREATE's own backoff schedule
    //       (m_nomination_next_create_at, armed by nomination_schedule_create()
    //       with a real 500/1000/2000/4000ms delay), so a retry scheduled
    //       after a TooFrequent create would never actually fire within this
    //       test. drain_membership() (already used by every retry-driving test
    //       above) calls debug_expire_create_spacing_for_test() instead, which
    //       expires all three of the pacer's deadlines -- exactly what is
    //       needed to drive a create retry to completion.
    {
        FakeTalkbackSdk sdk;
        sdk.script_create_results({TalkbackResult::TooFrequent,
                                   TalkbackResult::TooFrequent,
                                   TalkbackResult::Ok});
        // svc/host: unused by the seam itself (see (1) above); still required
        // so nominate() has a host to gate against.
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&sdk);
        tb.set_host(&host);

        std::vector<std::string> lines;
        EngineIpc::test_sink() = [&](const std::string &l) { lines.push_back(l); };
        check(tb.nominate(&host, {"Sarah"}), "nominate refused a one-name plan");
        drain_membership(tb);
        EngineIpc::test_sink() = nullptr;

        check(sdk.create_calls() == 3,
              "a too-frequent create was not retried -- Law 2 backoff is not "
              "reaching the ladder through the seam");
        // count_abort_reports_because(), not a bare substring search: the
        // diagnostic "create_rate_limited_retry" stage (expected here --
        // it is the "waiting, will retry" line for each of the two
        // TooFrequent results) contains "create_rate_limited" as a literal
        // prefix, so a plain line_has() search matches it too and cannot
        // tell "still retrying" from "gave up for this reason", which is
        // the exact distinction this assertion exists to draw.
        check(count_abort_reports_because(lines, "create_rate_limited") == 0,
              "a create that eventually succeeded was reported as rate-limited");
    }

    // -- Fix round 2 (Important 6): pin the SetEvent-failure refusal fix
    // round 1 restored -- probe()'s
    // `if (!m_sdk->events_registered()) return probe_refused_without_ladder();`.
    // Deleting that check left the whole suite green in fix round 1's own
    // verification, because nothing anywhere set events_registered_value
    // false; this is the honest fake-side hook that makes it pinnable. A
    // failed event registration means no callback will ever reach this
    // object, so the create must never be issued -- exactly the
    // pre-Task-1 behaviour (`if (set_err != SDKERR_SUCCESS) return
    // probe_refused_without_ladder();`) this restored. -----------------------
    {
        FakeMeetingService svc;
        FakeTalkbackHost host(svc);
        EngineTalkback tb;
        tb.set_sdk(&svc.ctrl);
        tb.set_host(&host);
        svc.ctrl.events_registered_value = false;
        check(!tb.probe(&host, "Someone"),
              "probe() proceeded after a failed event registration");
        check(svc.ctrl.creates == 0,
              "a probe with no callback sink still issued CreateChannel");
    }

    if (failures == 0)
        std::cout << "engine-talkback-select: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
