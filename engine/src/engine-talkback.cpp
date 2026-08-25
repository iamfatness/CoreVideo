#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::write -- an inline fn in a namespace,
                             // so it must be INCLUDED, never forward-declared
#include "talkback-tone.h"
#include "engine-json.h"     // zchar_to_utf8 / json_escape / json_str (Step 3a)
#include "talkback-ring.h"   // talkback_ring_drain / TalkbackRingSlotFn (Milestone 2)

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

void EngineTalkback::report(const std::string &stage, const std::string &fields) const
{
    std::string line = R"({"cmd":"talkback_probe","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

bool EngineTalkback::probe(ZOOMSDK::IMeetingService *svc,
                           const std::string &participant_name)
{
    // Re-entrancy guard: Task 5 wires this to a control-API command a human
    // can send twice. A second call while a channel is live would otherwise
    // unconditionally reset m_channel_id_z out from under the live ladder,
    // discarding the only handle we have to destroy it -- refusing is
    // correct for a probe; there is nothing sensible to queue or cancel.
    // Returning false here (rather than starting a ladder) is also the
    // caller's signal not to spawn a second tick()-driving thread -- see the
    // return-value comment on this function's declaration.
    const Phase current = m_phase.load(std::memory_order_acquire);
    if (current != Phase::Idle && current != Phase::Done) {
        report("busy", R"("phase":)" + std::to_string(static_cast<int>(current)));
        return false;
    }

    // R1 review-round fix (mutual exclusion): the probe and the persistent
    // session must never run concurrently. Before this, session_start()
    // reassigned m_svc/m_ctrl -- the exact same fields tick() dereferences on
    // its OWN driving thread while a probe is in flight -- with no gate at
    // all: a genuine cross-thread pointer race (Critical 2). Refusing here
    // rather than queueing also means tick()'s batch-destroy (for the
    // probe's channel) and session_stop()'s batch-destroy (for the session's
    // channel) can never overlap on the same controller object (Important
    // 4). A probe is a ~3s diagnostic; refusing it for the life of an active
    // talkback session costs nothing real -- do not "relax" this to allow
    // concurrent use. See the matching guard in session_start().
    if (m_session_live) {
        report("busy", R"("reason":"session_live")");
        return false;
    }

    m_svc = svc;
    m_participant_name = participant_name;
    m_phase.store(Phase::Idle, std::memory_order_release);
    {
        // Cross-thread: a late callback from the PREVIOUS probe can still be
        // reading these on the SDK thread right now -- see the m_chan_mtx
        // comment in the header.
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_channel_id.clear();
        m_channel_id_z.clear();
    }
    m_participant_id = 0;
    m_tone_index = 0;
    m_buffers_sent = 0;
    m_destroy_attempts = 0;

    if (!m_svc) {
        report("controller", R"("ok":false,"reason":"no_meeting_service")");
        m_phase.store(Phase::Done, std::memory_order_release);
        return true;
    }

    // RUNG 1: does the controller exist at all on this SDK/account?
    m_ctrl = m_svc->GetMeetingTalkbackController();
    report("controller", std::string(R"("ok":)") + (m_ctrl ? "true" : "false"));
    if (!m_ctrl) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return true;
    }

    // RUNG 2: the meeting-level gate. This is the one we expect Enhanced Media
    // to satisfy, and the one that decides whether the feature is viable.
    const bool supported = m_ctrl->IsMeetingSupportTalkBack();
    report("meeting_supported",
           std::string(R"("supported":)") + (supported ? "true" : "false"));
    if (!supported) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return true;
    }

    const ZOOMSDK::SDKError set_err = m_ctrl->SetEvent(this);
    report("set_event", R"("code":)" + std::to_string(static_cast<int>(set_err)));
    if (set_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return true;
    }

    // RUNG 3: create exactly one channel. Max 16 exist; we make one and
    // destroy it, so a failed probe cannot leak channel budget into the
    // meeting.
    //
    // Gate behind the same arbiter session_start() uses: exactly one create
    // may be outstanding across the probe and the session (see
    // src/talkback-channel-owner.h). Refuse rather than queue -- there is
    // nothing sensible to queue, and a queued create would arrive with the
    // other subsystem's response still in flight. m_pending_create is
    // guarded by m_chan_mtx (see the header comment on it) -- copy the
    // decision out under the lock, release, THEN call the SDK, same
    // discipline as every other m_chan_mtx access in this file.
    bool create_gate_ok;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        create_gate_ok = talkback_may_request_create(m_pending_create);
    }
    if (!create_gate_ok) {
        report("busy", R"("reason":"create_busy")");
        m_phase.store(Phase::Done, std::memory_order_release);
        return true;
    }
    const ZOOMSDK::SDKError create_err = m_ctrl->CreateChannel(1);
    report("create_channel", R"("code":)" +
           std::to_string(static_cast<int>(create_err)));
    if (create_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_pending_create = TalkbackChannelOwner::Probe;
    }
    m_phase_deadline.store(
        (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
        std::memory_order_release);
    m_phase.store(Phase::AwaitingChannel, std::memory_order_release);   // continues in onCreateChannelResponse
    return true;
}

void EngineTalkback::drain_stray_channels()
{
    // Only tick() calls this, and tick() is the sole caller of the
    // batch-destroy API -- see the invariant comment at the top of the
    // Destroying branch below. Swap the queue out under lock, then never
    // touch the SDK while holding m_chan_mtx.
    //
    // F6 review-round fix: m_ctrl can be null here. probe()'s RUNG 1
    // reassigns m_ctrl on EVERY call (`m_ctrl = m_svc->GetMeetingTalkback
    // Controller();`), so a LATER probe that fails to obtain a controller
    // nulls it out while an EARLIER ladder's stray, queued by a successful
    // probe, is still sitting in m_stray_channels waiting to be drained.
    // Guard before touching the queue at all, so a null m_ctrl leaves the
    // strays queued for a later tick() rather than swapping them out and
    // losing them silently -- losing a leaked-channel record silently is
    // exactly the failure mode this class exists to avoid.
    if (!m_ctrl) return;

    // R1-round-3 review fix (the has_pending_work() gap): has_pending_work()
    // used to infer "the driving thread might still touch the SDK" from
    // m_phase and m_stray_channels alone. Both can read "nothing to see
    // here" while THIS function is still live: the swap below empties the
    // m_stray_channels member immediately, and m_phase can independently
    // already be Done (a stray drained on the very tick() that settles the
    // ladder, or on a later call after the ladder already settled) -- so for
    // the entire window from here through the end of the SDK loop below,
    // has_pending_work() would report "safe to proceed" to session_start()
    // on the OTHER thread while this thread is still calling
    // m_ctrl->BeginBatchDestroyChannels() and friends. session_start()'s R1
    // mutual-exclusion gate depends on has_pending_work() being right about
    // that, so set the flag BEFORE the swap (not after -- the swap itself,
    // under m_chan_mtx, is part of the window a concurrent reader must see
    // as busy) and clear it via RAII so every exit from here on -- today
    // that's the empty-queue early return right below and falling off the
    // end of the loop, but a future edit adding another early return does
    // not have to remember to duplicate the clear.
    m_driving_thread_in_sdk_call.store(true, std::memory_order_release);
    struct ClearOnExit {
        std::atomic<bool> &flag;
        ~ClearOnExit() { flag.store(false, std::memory_order_release); }
    } clear_on_exit{m_driving_thread_in_sdk_call};

    std::vector<std::basic_string<zchar_t>> strays;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        if (m_stray_channels.empty()) return;
        strays.swap(m_stray_channels);
    }

    for (const auto &id : strays) {
        // Bounded, local retry -- deliberately not tied to m_destroy_attempts
        // (that counter is reserved for the main channel) and deliberately
        // not spread across future tick() calls, so one stray can never
        // grow into an unbounded retry loop.
        ZOOMSDK::SDKError e = ZOOMSDK::SDKERR_UNKNOWN;
        uint32_t attempt = 0;
        for (; attempt < kMaxDestroyAttempts; ++attempt) {
            e = m_ctrl->BeginBatchDestroyChannels();
            if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddChannelToDestroy(id.c_str());
            if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchDestroyChannels();
            if (e == ZOOMSDK::SDKERR_SUCCESS) break;
        }
        report("stray_destroy",
               R"("channel":")" + json_escape(zchar_to_utf8(id.c_str())) +
               R"(","code":)" + std::to_string(static_cast<int>(e)) +
               R"(,"attempts":)" + std::to_string(attempt + 1));
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            report("stray_destroy_abandoned",
                   R"("channel":")" + json_escape(zchar_to_utf8(id.c_str())));
        }
    }
}

void EngineTalkback::tick()
{
    // tick() is the ONLY caller of the batch-destroy API
    // (BeginBatchDestroyChannels / AddChannelToDestroy /
    // ExecuteBatchDestroyChannels). Callbacks that discover a channel
    // needing cleanup queue it via m_stray_channels; they never call the
    // API themselves. Two owners on two threads could interleave
    // Begin/Add/Execute against each other and corrupt or merge batches,
    // since the API shape implies the controller holds implicit per-batch
    // state -- that is the whole reason this design is safe, and it is not
    // visible anywhere else in the code.
    drain_stray_channels();

    const Phase phase = m_phase.load(std::memory_order_acquire);

    if (phase == Phase::AwaitingChannel || phase == Phase::AwaitingInvite) {
        // SDKERR_SUCCESS on CreateChannel/ExecuteBatchInviteUsers is only a
        // promise the call was accepted, not that onCreateChannelResponse /
        // onChannelUserJoinResponse will ever fire. Without this deadline a
        // swallowed callback hangs here forever and reports nothing -- a
        // hang IS silence, and it is indistinguishable from a permission
        // failure at exactly the moment we need to tell the two apart.
        const auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(
                m_phase_deadline.load(std::memory_order_acquire)));
        if (std::chrono::steady_clock::now() >= deadline) {
            report("timeout", R"("phase":)" + std::to_string(static_cast<int>(phase)));
            // R3 review-round fix: an AwaitingChannel timeout means the
            // CreateChannel THIS probe issued was accepted but its response
            // never arrived -- that response is what would normally clear
            // m_pending_create (via the arbiter in onCreateChannelResponse).
            // Left set, it wedges every future CreateChannel from EITHER
            // subsystem for the rest of the process's life, and it wedges
            // at exactly the moment the SDK is already misbehaving -- the
            // one time this diagnostic most needs to stay usable. Only
            // AwaitingChannel, never AwaitingInvite: by AwaitingInvite the
            // create already succeeded and onCreateChannelResponse already
            // cleared this. This is a completion of the existing timeout,
            // not a change to its Phase transitions or the re-entrancy
            // guard -- both are untouched. Runs on the probe's OWN driving
            // thread (this function's caller), unlike every other writer of
            // m_pending_create, so it goes through m_chan_mtx like the
            // channel-id strings do -- see the header comment on the field.
            if (phase == Phase::AwaitingChannel) {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                if (m_pending_create == TalkbackChannelOwner::Probe)
                    m_pending_create = TalkbackChannelOwner::None;
            }
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

        std::basic_string<zchar_t> channel_copy;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            channel_copy = m_channel_id_z;
        }

        const ZOOMSDK::SDKError e = m_ctrl->SendAudioDataToChannel(
            channel_copy.c_str(), reinterpret_cast<const char *>(pcm),
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
        // prevent. This call site and drain_stray_channels() above are the
        // only two places in this file allowed to touch the batch-destroy
        // API, and both run here, on tick()'s thread -- see the comment at
        // the top of tick().
        std::basic_string<zchar_t> channel_copy;
        std::string channel_copy_utf8;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            channel_copy = m_channel_id_z;
            channel_copy_utf8 = m_channel_id;
        }

        ZOOMSDK::SDKError e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS && !channel_copy.empty())
            e = m_ctrl->AddChannelToDestroy(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->ExecuteBatchDestroyChannels();
        report("destroy", R"("code":)" + std::to_string(static_cast<int>(e)) +
               R"(,"attempt":)" + std::to_string(m_destroy_attempts + 1));

        if (e == ZOOMSDK::SDKERR_SUCCESS) {
            // F8 review-round fix: m_channel_id_z used to be cleared only at
            // the start of the NEXT probe(), never here -- so the probe's
            // throwaway channel id outlived the channel itself. If the audio
            // path (open_audio/drain_audio) is live at the same time, every
            // subsequent SendAudioDataToChannel targets a destroyed channel,
            // fails every buffer, and (before the drain_audio rate-limit
            // fixed elsewhere in this round) reported it on every drain --
            // the same message-storm shape this codebase already has a live
            // incident about. Clear both representations together, under the
            // same lock discipline as every other m_channel_id_z access.
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                m_channel_id.clear();
                m_channel_id_z.clear();
            }
            m_phase.store(Phase::Done, std::memory_order_release);
            return;
        }

        if (++m_destroy_attempts >= kMaxDestroyAttempts) {
            // Out of retries: the channel is now stranded for the rest of the
            // meeting (one of 16 gone for good) and nothing will self-heal
            // it. Make that visible rather than silently giving up.
            report("destroy_abandoned",
                   R"("channel":")" + json_escape(channel_copy_utf8) +
                   R"(","attempts":)" + std::to_string(m_destroy_attempts));
            // F8 review-round fix: same clearing as the success path above.
            // "Abandoned" means WE stop tracking it -- Zoom's copy of the
            // channel may or may not still exist, but this probe has given
            // up on it either way, and a stale id here causes exactly the
            // same audio_send failure storm as the success path.
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                m_channel_id.clear();
                m_channel_id_z.clear();
            }
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
        if (zchar_to_utf8(u->GetUserName()) == name) {
            // F2 review-round fix: IsSupportTalkback() is a PER-USER gate,
            // distinct from IsMeetingSupportTalkBack() (the meeting-level
            // gate reported at RUNG 2 in probe()) -- a meeting can support
            // talkback while this specific participant's client cannot
            // receive it. We report it rather than refuse to proceed on
            // false: refusing would collapse the exact distinction this
            // probe exists to draw. Without this, "all rungs green, sent
            // buffers=300, talent heard nothing" and "talent's client
            // cannot receive talkback at all" are indistinguishable from
            // the log -- reporting supported=false and still attempting
            // the send is what makes the contrast between this value and
            // what the human actually hears into data.
            const bool supported = u->IsSupportTalkback();
            report("participant_talkback_support",
                   R"("name":")" + json_escape(name) + R"(","user_id":)" +
                   std::to_string(uid) + R"(,"supported":)" +
                   (supported ? "true" : "false"));
            return uid;
        }
    }
    return 0;
}

void EngineTalkback::onCreateChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    const std::string id = zchar_to_utf8(channelID);
    report("create_channel_response",
           R"("channel":")" + json_escape(id) + R"(","error":)" +
           std::to_string(static_cast<int>(error)));

    // Route by who asked. See src/talkback-channel-owner.h: the response
    // carries no indication of its requester, so the arbiter is the only
    // thing standing between the probe and the session adopting each other's
    // channels. Claim and clear happen in the SAME lock scope -- once this
    // response has been attributed to an owner, m_pending_create must not be
    // observable as still "theirs" by anyone else, even for the instant
    // between a separate claim and a separate clear.
    TalkbackChannelOwner owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        owner = talkback_claim_create(m_pending_create);
        if (owner != TalkbackChannelOwner::None)
            m_pending_create = TalkbackChannelOwner::None;
    }
    if (owner == TalkbackChannelOwner::Session) {
        if (error != TALKBACK_ERROR_OK || channelID == nullptr) {
            report("session_channel", R"("ok":false,"error":)" +
                   std::to_string(static_cast<int>(error)));
            return;
        }
        // Minor review-round fix: keep the value from THIS lock scope
        // instead of re-locking immediately after just to read back what was
        // just written -- channel_copy is exactly m_session_channel_z at
        // this point, so a second lock/unlock pair bought nothing.
        std::basic_string<zchar_t> channel_copy;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            m_session_channel_z.assign(channelID);
            m_session_channel = zchar_to_utf8(channelID);
            channel_copy = m_session_channel_z;
        }
        // Invite by NAME, resolved now: Zoom user ids are meeting-scoped, so
        // a stored id points at nobody after a rejoin and at the wrong face
        // once ids are recycled.
        m_session_user_id = resolve_participant(m_session_participant);
        if (m_session_user_id == 0) {
            report("session_invite", R"("ok":false,"reason":"no_participant_named",)"
                   R"("name":")" + json_escape(m_session_participant) + "\"");
            session_stop();
            return;
        }
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_session_user_id);
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
        report("session_invite", R"("user_id":)" + std::to_string(m_session_user_id) +
               R"(,"code":)" + std::to_string(static_cast<int>(e)));
        if (e != ZOOMSDK::SDKERR_SUCCESS) { session_stop(); return; }
        m_ctrl->SetChannelBackgroundVolume(channel_copy.c_str(), 0.3f);
        m_session_live = true;
        report("session_live", R"("channel":")" + json_escape(m_session_channel) + "\"");
        return;
    }
    // owner == Probe was already cleared above; owner == None means nothing
    // was outstanding (a stray/duplicate, handled by id-comparison below --
    // never by m_pending_create, which by definition has no opinion here).

    if (!channelID) {
        // F1 review-round fix: guard before ANY comparison or assignment
        // touches channelID as a raw pointer -- std::basic_string's
        // operator== / assign() against a null zchar_t* is
        // char_traits::length(nullptr), an access violation. zchar_to_utf8
        // above already null-guards internally (that's the precedent this
        // follows), but the `m_channel_id_z == channelID` comparison and
        // the `m_stray_channels.emplace_back(channelID)` construction a few
        // lines down do not, and a null id is reachable here on any error
        // code, not only TALKBACK_ERROR_OK. Report it distinctly -- we want
        // to KNOW the SDK did this -- rather than crash or silently no-op.
        report("create_channel_response_null_channel",
               R"("error":)" + std::to_string(static_cast<int>(error)));
        if (m_phase.load(std::memory_order_acquire) == Phase::AwaitingChannel) {
            // This was the rung we were waiting on and it came back with
            // nothing usable -- same disposition as any other
            // create_channel failure. If it was a stray/duplicate for a
            // channel that isn't the live one, there is no id to queue for
            // cleanup, so there is nothing else to do.
            m_phase.store(Phase::Done, std::memory_order_release);
        }
        return;
    }

    if (m_phase.load(std::memory_order_acquire) != Phase::AwaitingChannel) {
        // Not the callback we're waiting on -- either a stray/duplicate, or
        // (now that tick() times AwaitingChannel out) a genuinely late
        // response that arrived after we already gave up on this rung and
        // moved on, in which case a real channel exists that our own ladder
        // no longer tracks. tick() is the ONLY caller of the batch-destroy
        // API (see the comment there) -- this callback runs on the SDK
        // thread and must never call BeginBatchDestroyChannels/etc directly,
        // even for cleanup, or it can interleave with tick()'s own
        // Begin/Add/Execute sequence and corrupt or merge batches. So: queue,
        // never call. The comparison against m_channel_id_z below is exactly
        // the access that must go through m_chan_mtx -- reachable while
        // phase is Idle/Done, i.e. while a fresh probe() is allowed to be
        // clearing/reassigning that member on another thread right now; see
        // the m_chan_mtx comment in the header.
        //
        // R2 review-round fix: a channel we OWN -- the probe's live one OR
        // the session's -- is never a stray, no matter what m_pending_create
        // said at routing time above. This is the fix for a redelivered
        // response: the SDK can and does redeliver onCreateChannelResponse
        // (that's what the *_duplicate / *_stray handling in this whole
        // block is for), and a redelivery can arrive long after the request
        // that caused it was already resolved -- at which point
        // m_pending_create has moved on to None or to the OTHER subsystem,
        // so the arbiter above no longer has an opinion about it. Comparing
        // ONLY against m_channel_id_z (as this branch used to) meant a
        // redelivered SESSION response landed here, failed that comparison,
        // and got queued as a stray -- so tick() destroyed the LIVE session
        // channel out from under drain_audio() on the probe's own thread.
        // That was the exact race this whole task exists to make
        // structurally impossible; this id check is what actually closes it
        // for the redelivery case (R1's mutual exclusion above closes the
        // DIFFERENT hazard of a concurrent request, not this one).
        if (error == TALKBACK_ERROR_OK) {
            bool is_probe_channel;
            bool is_session_channel;
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                is_probe_channel   = (m_channel_id_z == channelID);
                is_session_channel = !is_probe_channel &&
                                     (m_session_channel_z == channelID);
                if (!is_probe_channel && !is_session_channel) {
                    // A genuinely different, untracked channel now exists.
                    // Queue it (still under the lock, so the check and the
                    // push are one atomic decision); drain_stray_channels()
                    // (called from tick()) owns actually destroying it.
                    m_stray_channels.emplace_back(channelID);
                }
            }
            if (is_session_channel) {
                // Matches the session's live channel -- a duplicate/
                // redelivered callback for the create the session already
                // finished handling. Queuing it for destroy would tear down
                // the channel drain_audio() is actively sending on. Report
                // and do NOTHING else.
                report("create_channel_response_session_duplicate",
                       R"("channel":")" + json_escape(id) + "\"");
            } else if (is_probe_channel) {
                // This id matches OUR live channel -- a duplicate/redelivered
                // callback for the channel the ladder already moved past
                // AwaitingChannel with (e.g. now mid-invite or mid-send).
                // Queuing it for destroy would tear down a channel a running
                // probe still depends on. Report and do NOTHING else.
                report("create_channel_response_duplicate",
                       R"("channel":")" + json_escape(id) + "\"");
            } else {
                report("create_channel_response_stray",
                       R"("channel":")" + json_escape(id) + R"(","queued":true)");
            }
        }
        return;
    }
    if (error != TALKBACK_ERROR_OK) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_channel_id = id;              // UTF-8, reporting only
        m_channel_id_z.assign(channelID); // SDK identifier, verbatim -- see header
    }

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

    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_channel_id_z;
    }
    ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channel_copy.c_str());
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_participant_id);
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
    report("invite", R"("user_id":)" + std::to_string(m_participant_id) +
           R"(,"code":)" + std::to_string(static_cast<int>(e)));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }
    m_phase_deadline.store(
        (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
        std::memory_order_release);
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

    if (!channelID) {
        // F1 review-round fix: this line is reached for EVERY error code,
        // including TALKBACK_ERROR_NOPERMISSION -- exactly the error this
        // probe exists to exercise -- and the SDK can hand back a null
        // channelID on that path. Without this guard, `channel_copy !=
        // channelID` below is std::basic_string comparing against a null
        // zchar_t*, i.e. char_traits::length(nullptr): an access violation
        // that replaces our diagnostic with a crash-recovery event at
        // exactly the moment we're trying to learn something. Report it
        // distinctly (we want to KNOW the SDK did that) and destroy the
        // channel we know we created -- m_channel_id_z is still valid; only
        // this callback's echo of it is null.
        report("invite_response_null_channel",
               R"("user_id":)" + std::to_string(userID) +
               R"(,"error":)" + std::to_string(static_cast<int>(error)));
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }

    // Copy both channel-id representations out under the lock before using
    // either -- same discipline as everywhere else m_channel_id_z is
    // touched, see the m_chan_mtx comment in the header.
    std::basic_string<zchar_t> channel_copy;
    std::string channel_copy_utf8;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_channel_id_z;
        channel_copy_utf8 = m_channel_id;
    }

    // Phase alone can't tell "our invite landed" from "some other invite
    // response landed while we happened to be in this phase" -- correlate
    // the callback to the exact channel and user we are waiting on.
    if (channel_copy != channelID || userID != m_participant_id) {
        report("invite_response_mismatch",
               R"("expected_channel":")" + json_escape(channel_copy_utf8) +
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
        m_ctrl->SetChannelBackgroundVolume(channel_copy.c_str(), 0.3f);
    report("background_volume", R"("code":)" +
           std::to_string(static_cast<int>(vol)));

    m_tone_index = 0;
    m_buffers_sent = 0;
    // Unlike m_phase, m_channel_id_z needs no ordering argument here: tick()
    // will re-read it through m_chan_mtx itself once it observes Sending
    // (see the m_chan_mtx comment in the header), so this store only needs
    // to publish the phase transition, not the string.
    m_phase.store(Phase::Sending, std::memory_order_release);   // tick() takes it from here
}

void EngineTalkback::onChannelUserLeaveResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onJoinTalkbackChannel(unsigned int) {}
void EngineTalkback::onLeaveTalkbackChannel(unsigned int) {}
void EngineTalkback::onInviterAudioLevel(unsigned int, unsigned int) {}

// ── Talkback audio path (Milestone 2) ───────────────────────────────────────
//
// The probe above (Milestone 1) only asked "can this account open a channel
// and put audio in it". This is the consuming half: the plugin's OBS tap
// writes director audio into a ring (src/talkback-ring.h), and this class
// maps that region, drains it, and forwards every buffer to Zoom via
// SendAudioDataToChannel on whatever channel this class already holds.
//
// THREADING: open_audio/drain_audio/close_audio are called ONLY from
// engine/src/main.cpp's command loop, which on Windows is ALSO the SDK's
// message-pump thread (see ipc_read_line_with_message_pump there). That is
// deliberate: every SDK call stays on the thread the SDK already uses. This
// is a correction of the Milestone 1 probe, which introduced a separate
// driving thread (still used by tick(), an accepted narrower exception) and
// was the first code in this engine to call SDK APIs off the pump. There is
// no need to repeat that here: the ring already decouples the OBS audio
// thread (the writer) from us, so nothing on this path needs to run
// off-thread to stay responsive -- it only needs to run on the thread the
// SDK expects.
bool EngineTalkback::open_audio(const std::string &region_name,
                                uint32_t sample_rate, uint16_t channels)
{
    close_audio();
    if (!shm_region_open_readwrite(
            m_audio_region, region_name,
            shm_audio_region_bytes(kTalkbackSlotBytes))) {
        report("audio_open", R"("ok":false,"reason":"map_failed","region":")" +
               json_escape(region_name) + "\"");
        return false;
    }

    auto *hdr = static_cast<ShmAudioHeader *>(m_audio_region.ptr);

    // F3 review-round fix: the ring's physical layout (slot_count,
    // slot_bytes) is whatever the WRITER laid down when it created the
    // region, but shm_audio_region_bytes() above sized OUR mapping from the
    // READER's own kTalkbackSlotBytes constant. shm_audio_slot_offset(*hdr,
    // index) multiplies by the WRITER's hdr->slot_bytes -- so a half-applied
    // install (CLAUDE.md: "a DLL-only copy silently half-applies" is routine
    // here) pairing an old engine against a plugin built with a larger
    // kTalkbackSlotBytes would walk that offset past the end of a mapping
    // sized for the smaller constant: an access violation, not a
    // diagnosable error. The header carries no version field, so this check
    // is the only thing standing between that version skew and a crash.
    // Requiring the exact slot_count also rules out slot_count == 0, which
    // would otherwise make every `% hdr->slot_count` in the drain path
    // (talkback-ring.h) a division by zero.
    if (hdr->slot_count != kAudioRingSlots || hdr->slot_bytes != kTalkbackSlotBytes) {
        report("audio_open",
               R"("ok":false,"reason":"layout_mismatch","slot_count":)" +
               std::to_string(hdr->slot_count) + R"(,"expected_slot_count":)" +
               std::to_string(kAudioRingSlots) + R"(,"slot_bytes":)" +
               std::to_string(hdr->slot_bytes) + R"(,"expected_slot_bytes":)" +
               std::to_string(kTalkbackSlotBytes) + R"(,"region":")" +
               json_escape(region_name) + "\"");
        shm_region_destroy(m_audio_region);
        m_audio_region = ShmRegion{};
        return false;
    }

    // F7 review-round fix: rate/channels used to come straight from the
    // pipe's talkback_open JSON, unvalidated -- json_uint() returns 0 for a
    // missing key, so a malformed talkback_open would send sampleRate = 0
    // straight to Zoom. The ring header is the AUTHORITATIVE copy: it is
    // written by talkback_ring_init() in the same call that lays out the
    // region (src/talkback-tap.cpp's open()), which already validated rate
    // and channel count before creating the region at all. Treat the pipe's
    // values as a cross-check against that authority, not a second source of
    // truth -- a mismatch means the plugin and the talkback_open command
    // disagree about which region/session this is, which is exactly the
    // kind of stale-handshake bug that must refuse loudly rather than send
    // audio at a value nobody actually asked for.
    const uint32_t hdr_rate = hdr->sample_rate;
    const uint16_t hdr_channels = static_cast<uint16_t>(hdr->channels);

    if (!talkback_pcm_rate_supported(hdr_rate)) {
        report("audio_open",
               R"("ok":false,"reason":"unsupported_rate","rate":)" +
               std::to_string(hdr_rate) + R"(,"region":")" +
               json_escape(region_name) + "\"");
        shm_region_destroy(m_audio_region);
        m_audio_region = ShmRegion{};
        return false;
    }
    if (hdr_channels != 1 && hdr_channels != 2) {
        report("audio_open",
               R"("ok":false,"reason":"unsupported_channels","channels":)" +
               std::to_string(hdr_channels) + R"(,"region":")" +
               json_escape(region_name) + "\"");
        shm_region_destroy(m_audio_region);
        m_audio_region = ShmRegion{};
        return false;
    }
    if (hdr_rate != sample_rate || hdr_channels != channels) {
        report("audio_open",
               R"("ok":false,"reason":"pipe_header_mismatch","pipe_rate":)" +
               std::to_string(sample_rate) + R"(,"header_rate":)" +
               std::to_string(hdr_rate) + R"(,"pipe_channels":)" +
               std::to_string(channels) + R"(,"header_channels":)" +
               std::to_string(hdr_channels) + R"(,"region":")" +
               json_escape(region_name) + "\"");
        shm_region_destroy(m_audio_region);
        m_audio_region = ShmRegion{};
        return false;
    }

    m_audio_region_name = region_name;
    m_audio_rate        = hdr_rate;
    m_audio_channels    = hdr_channels;
    // Start at the writer's CURRENT index, not 0: buffers published before we
    // mapped are stale by definition, and replaying them would put a burst of
    // old audio in the channel the moment a key opens.
    m_audio_read_index = hdr->write_index;

    // F1 review-round fix (CRITICAL): the tap's capture callback attaches
    // and starts publishing as soon as TalkbackTap::open() runs on the
    // plugin side, which is BEFORE this engine ever handles talkback_open --
    // the pipe round-trip guarantees that ordering. audio_ring_notify_after_
    // publish() (engine-ipc.h) only sends an event on the empty->non-empty
    // edge, so by the time we map this region `notify` is very likely
    // already 1 from a buffer published in that window. Unlike the MAIN
    // audio ring, this ring has NO keepalive (no ~2.5s / 250-buffer
    // self-heal) to fall back on: if we leave notify=1 here, the writer
    // never re-notifies (it only notifies on an edge it never sees again
    // while the ring keeps being non-empty), drain_audio() is never called,
    // and the ring silently fills and laps for the rest of the region's
    // life -- the director keys and nothing is heard, with no diagnostic
    // anywhere. Abandon unconditionally, snapshot FIRST: a buffer published
    // in the race window between the snapshot above and this call is still
    // ahead of m_audio_read_index and will still be picked up by the next
    // drain_audio(); abandoning only clears the flag so the NEXT publish
    // after this point is guaranteed to (re-)cross the edge and notify.
    audio_ring_reader_abandon(hdr);

    m_audio_open = true;
    m_audio_send_fail_count = 0; // F8: fresh session, fresh report budget
    report("audio_open", R"("ok":true,"rate":)" + std::to_string(m_audio_rate) +
           R"(,"channels":)" + std::to_string(m_audio_channels));
    return true;
}

namespace {
struct SendCtx {
    ZOOMSDK::IMeetingTalkbackController *ctrl;
    const zchar_t *channel;
    uint32_t rate;
    ZOOMSDK::ZoomSDKAudioChannel chan;
    uint32_t sent;
    uint32_t no_channel_drops;   // buffers seen while ctrl/channel is unset
    int last_err;
};

void send_one(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<SendCtx *>(ctx);
    // No channel yet (no probe has established one in this session, or a
    // future channel-selection milestone has not run) -- there is nowhere to
    // send this buffer. That is a real choice, not a bug, but per this
    // codebase's rule against silent audio loss it must still be counted,
    // not just dropped -- see the no_channel_drops report below.
    if (!c->ctrl || !c->channel) { ++c->no_channel_drops; return; }
    const ZOOMSDK::SDKError e = c->ctrl->SendAudioDataToChannel(
        c->channel, static_cast<const char *>(pcm), byte_len, c->rate, c->chan);
    if (e != ZOOMSDK::SDKERR_SUCCESS) c->last_err = static_cast<int>(e);
    ++c->sent;
}
} // namespace

void EngineTalkback::drain_audio()
{
    // Only bail before touching the ring when there is no valid mapping to
    // touch -- m_ctrl / an established channel being unset is NOT one of
    // these reasons. See below: a missing channel still drains (and
    // discards, loudly) rather than returning early, because returning
    // early here would leave `notify` set on a ring that DOES have pending
    // data -- the writer only re-notifies on the empty->non-empty edge, so
    // an early return here would silence talkback the moment a key opens
    // before the first channel is ever established, with no self-healing
    // short of the writer recreating the region. That is exactly the
    // failure class the edge-triggered protocol's helpers exist to rule
    // out (see ShmAudioHeader::notify in src/engine-ipc.h).
    if (m_audio_region.ptr == nullptr) return;

    // F1 review-round fix: !m_audio_open with a still-mapped region is a
    // separate case from "nothing to touch" above, and it must not be
    // handled the same bare-return way. A bare return here neither drains
    // nor abandons -- it just does nothing, which per the comment above is
    // exactly the "consumed a wakeup and left the flag set" shape: if
    // `notify` happens to already be 1 (a plausible state, e.g. reached via
    // any future path that maps the region before flipping m_audio_open),
    // this call silently eats the opportunity to clear it, and the writer's
    // edge-triggered protocol has no way to know a reader ever looked. Hand
    // the flag back explicitly instead of doing nothing.
    if (!m_audio_open) {
        audio_ring_reader_abandon(static_cast<ShmAudioHeader *>(m_audio_region.ptr));
        return;
    }

    // The channel to talk on is the SESSION's channel (Milestone 5), not the
    // probe's: the probe's m_channel_id_z is destroyed by tick() from a
    // separate thread three seconds after it opens, so sending on it here
    // would race that destroy mid-SendAudioDataToChannel. The session owns
    // its own channel for exactly this reason -- see the comment on
    // session_start()/session_stop() -- so tick() can never touch what this
    // function sends on; the race is now structurally impossible rather than
    // merely unlikely. No channel id crosses the IPC boundary and nothing
    // here needs a UTF-8 -> zchar_t conversion.
    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_session_channel_z;
    }

    auto *hdr = static_cast<ShmAudioHeader *>(m_audio_region.ptr);

    SendCtx ctx{m_ctrl, channel_copy.empty() ? nullptr : channel_copy.c_str(),
                m_audio_rate,
                m_audio_channels > 1 ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                                     : ZOOMSDK::ZoomSDKAudioChannel_Mono,
                0, 0, 0};

    // EVENTS ARE PROMPTS, NOT PAYLOADS: drain everything available, then use
    // the reader helpers to decide whether sleeping is safe. Any return path
    // that consumes a wakeup and leaves notify set silences talkback until the
    // writer's next edge -- the failure that silenced whole sources before the
    // helpers existed. This loop runs unconditionally, whether or not a
    // channel is currently held (see the comment above): draining is what
    // resolves `notify`, sending audio through it is a separate concern.
    uint32_t lost = 0;
    for (int pass = 0; pass < 4; ++pass) {
        talkback_ring_drain(m_audio_region.ptr, m_audio_read_index,
                            send_one, &ctx, &lost);
        if (audio_ring_reader_done(hdr, m_audio_read_index)) break;
        if (pass == 3) audio_ring_reader_abandon(hdr);
    }

    if (ctx.last_err != 0 || lost != 0 || ctx.no_channel_drops != 0) {
        // F8 review-round fix: this used to report on every drain_audio()
        // call that saw any failure. drain_audio() runs on every
        // talkback_audio pipe line -- with a stale channel id (the destroy-
        // path bug fixed elsewhere in this round) or a channel that simply
        // never got established, EVERY buffer fails and this fired
        // unbounded: ~50-100 pipe lines/sec, the same message-storm shape
        // this codebase already has a live incident about (the probe's own
        // "send" report in tick() guards against exactly this, for the same
        // reason). Report the first occurrence, then only periodically.
        ++m_audio_send_fail_count;
        if (m_audio_send_fail_count == 1 || (m_audio_send_fail_count % 100) == 0) {
            report("audio_send", R"("code":)" + std::to_string(ctx.last_err) +
                   R"(,"buffers":)" + std::to_string(ctx.sent) +
                   R"(,"lost":)" + std::to_string(lost) +
                   R"(,"no_channel_drops":)" + std::to_string(ctx.no_channel_drops) +
                   R"(,"occurrence":)" + std::to_string(m_audio_send_fail_count));
        }
    }
}

// ── Persistent talkback session (Milestone 5) ───────────────────────────────
//
// Deliberately NOT part of the probe's Phase machine above: that machine
// exists to tear itself down after one tone, which is the opposite of what a
// key held down needs. The session owns its OWN channel (m_session_channel_z,
// distinct from the probe's m_channel_id_z), so tick() -- which destroys the
// PROBE's channel from a separate thread -- can never touch it. That
// separation is what makes the SendAudioDataToChannel/destroy race
// structurally impossible rather than merely unlikely.
bool EngineTalkback::session_live() const { return m_session_live; }

bool EngineTalkback::session_start(ZOOMSDK::IMeetingService *svc,
                                   const std::string &participant_name)
{
    if (m_session_live) {
        report("session_start", R"("ok":false,"reason":"already_live")");
        return false;
    }

    // R1 review-round fix (mutual exclusion): refuse while the probe's
    // driving thread might still be running -- see the matching guard at the
    // top of probe(), which explains why. This check MUST come before
    // m_svc/m_ctrl are touched below: those are the exact fields tick()
    // dereferences on its own thread for as long as has_pending_work() is
    // true, and reassigning them here while that thread is mid-call was
    // Critical 2 -- a genuine cross-thread pointer race, not merely a
    // logical ordering nit. See has_pending_work()'s doc comment in the
    // header for why phase-alone (is_idle()) is not the right check: a
    // queued-but-undrained stray still means tick() has work left to do.
    if (has_pending_work()) {
        report("session_start", R"("ok":false,"reason":"probe_busy")");
        return false;
    }

    if (!svc) {
        report("session_start", R"("ok":false,"reason":"not_in_meeting")");
        return false;
    }
    m_svc  = svc;
    m_ctrl = m_svc->GetMeetingTalkbackController();
    if (!m_ctrl) {
        report("session_start", R"("ok":false,"reason":"no_controller")");
        return false;
    }
    if (!m_ctrl->IsMeetingSupportTalkBack()) {
        report("session_start", R"("ok":false,"reason":"not_supported")");
        return false;
    }
    m_ctrl->SetEvent(this);

    // m_pending_create is guarded by m_chan_mtx -- see the header comment on
    // it. Copy the decision out under the lock, release, THEN call the SDK.
    bool create_gate_ok;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        create_gate_ok = talkback_may_request_create(m_pending_create);
    }
    if (!create_gate_ok) {
        // The probe is mid-create. Refuse rather than queue: a queued create
        // would arrive with the other subsystem's response still in flight,
        // which is exactly the ambiguity the arbiter exists to remove.
        report("session_start", R"("ok":false,"reason":"create_busy")");
        return false;
    }

    m_session_participant = participant_name;
    const ZOOMSDK::SDKError e = m_ctrl->CreateChannel(1);
    report("session_start", R"("code":)" + std::to_string(static_cast<int>(e)) +
           R"(,"participant":")" + json_escape(participant_name) + "\"");
    if (e != ZOOMSDK::SDKERR_SUCCESS) return false;

    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_pending_create = TalkbackChannelOwner::Session;
    }
    return true;
}

void EngineTalkback::session_stop()
{
    if (!m_session_live && m_session_channel_z.empty()) {
        // Nothing to tear down. Still clear the pending create so a refused
        // start cannot wedge the arbiter. m_pending_create is guarded by
        // m_chan_mtx -- see the header comment on it.
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        if (m_pending_create == TalkbackChannelOwner::Session)
            m_pending_create = TalkbackChannelOwner::None;
        return;
    }

    std::basic_string<zchar_t> channel_copy;
    std::string channel_copy_utf8;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy      = m_session_channel_z;
        channel_copy_utf8 = m_session_channel;
        m_session_channel_z.clear();
        m_session_channel.clear();
        if (m_pending_create == TalkbackChannelOwner::Session)
            m_pending_create = TalkbackChannelOwner::None;
    }
    m_session_live    = false;
    m_session_user_id = 0;

    if (channel_copy.empty() || !m_ctrl) {
        report("session_stop", R"("ok":true,"reason":"no_channel")");
        return;
    }

    // The session destroys its OWN channel here, on the command-loop thread.
    // This is NOT the batch-destroy path tick() owns for the probe's stray
    // queue -- keeping them separate is what keeps "tick() is the sole
    // caller of the batch-destroy API for the probe's stray queue" true.
    // R1's mutual exclusion (see probe()/session_start()) guarantees tick()
    // can never be mid-Destroying (or mid-drain_stray_channels) at the same
    // time this runs, so there is no Begin/Add/Execute interleaving between
    // the two despite both calling the batch-destroy API on the same
    // controller object -- the hazard tick()'s own top-of-function comment
    // warns about is about two THREADS doing that concurrently, and R1 rules
    // that out here by construction.
    //
    // R4 review-round fix: this used to call Begin/Add/Execute exactly once
    // and report whatever code came back -- unlike every OTHER destroy path
    // in this file (tick()'s Destroying phase, drain_stray_channels()), both
    // of which retry up to kMaxDestroyAttempts times before giving up. This
    // file's own doctrine (stated at both of those call sites) is that a
    // leaked channel consumes one of the meeting's 16 for the rest of the
    // meeting; a single transient failure here silently leaked the
    // session's channel the same way. Match the existing retry pattern
    // instead of inventing a third variant.
    ZOOMSDK::SDKError e = ZOOMSDK::SDKERR_UNKNOWN;
    uint32_t attempt = 0;
    for (; attempt < kMaxDestroyAttempts; ++attempt) {
        e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->AddChannelToDestroy(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->ExecuteBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS) break;
    }
    report("session_stop", R"("code":)" + std::to_string(static_cast<int>(e)) +
           R"(,"attempts":)" + std::to_string(attempt + 1));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        report("session_destroy_abandoned",
               R"("channel":")" + json_escape(channel_copy_utf8) + "\"");
    }
}

void EngineTalkback::close_audio()
{
    if (!m_audio_open) return;
    m_audio_open = false;
    if (m_audio_region.ptr) {
        // Hand the flag back so the writer re-notifies rather than assuming a
        // reader is still listening.
        audio_ring_reader_abandon(static_cast<ShmAudioHeader *>(m_audio_region.ptr));
    }
    shm_region_destroy(m_audio_region);
    m_audio_region = ShmRegion{};
    m_audio_read_index = 0;
    report("audio_close", R"("ok":true)");
}
