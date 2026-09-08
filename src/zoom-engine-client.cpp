#include "zoom-engine-client.h"
#include "speaker-director.h"
#include "talkback-nomination-dispatch.h" // Task 5 fix round 2, N5
#include "talkback-key.h"  // talkback_session_mic_blocked() -- Law 1
#include "talkback-plan.h" // talkback_dedup_preserve_order() -- Task 5 fix round 1, F4
#include "zoom-join-decision.h"
#include "zoom-privilege-notice.h"
#include "zoom-engine-error-dispatch.h" // record-privilege handshake copy/classification
#include "zoom-reconnect.h"
#include "zoom-sdk-init-retry.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <obs-module.h>
#include <util/platform.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#if defined(WIN32)
#include <tlhelp32.h> // stale-engine sweep in terminate_stale_engine_processes()
#endif

bool cv_zoom_verbose_logging()
{
    static const bool verbose = [] {
        const char *v = std::getenv("CV_ZOOM_VERBOSE_LOG");
        return v && *v && !(v[0] == '0' && v[1] == '\0');
    }();
    return verbose;
}

// Stages emitted per video/audio frame or per roster tick — the bulk of the
// log volume. Kept out of the OBS log unless verbose; always still recorded
// in the diagnostics ring buffer.
static bool is_high_frequency_stage(const QString &stage)
{
    return stage == QLatin1String("video_frame_received") ||
           stage == QLatin1String("audio_frame_received") ||
           stage == QLatin1String("audio_one_way_frame_received") ||
           stage == QLatin1String("audio_target_added") ||
           stage == QLatin1String("share_frame_received") ||
           stage == QLatin1String("set_resolution") ||
           stage == QLatin1String("video_subscribe_noop_existing") ||
           stage == QLatin1String("video_raw_status");
}

static bool is_permanent_meeting_failure(int code)
{
    switch (code) {
    case 4:   // MEETING_FAIL_PASSWORD_ERR
    case 6:   // MEETING_FAIL_MEETING_OVER
    case 8:   // MEETING_FAIL_MEETING_NOT_EXIST
    case 9:   // MEETING_FAIL_MEETING_USER_FULL
    case 12:  // MEETING_FAIL_CONFLOCKED
    case 13:  // MEETING_FAIL_MEETING_RESTRICTED
    case 23:  // MEETING_FAIL_ENFORCE_LOGIN
    case 60:  // MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING
    case 62:  // MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN
    case 63:  // MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING
    case 64:  // MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN
    case 82:  // MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING
    case 500: // MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR
    case 501: // MEETING_FAIL_AUTHORIZED_USER_NOT_INMEETING
    case 502: // MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR
    case 503: // MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF
    case 504: // MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING
    case 505: // MEETING_FAIL_ON_BEHALF_TOKEN_INVALID
    case 506: // MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING
    case 1143: // MEETING_FAIL_JMAK_USER_EMAIL_NOT_MATCH
        return true;
    default:
        return false;
    }
}

static std::string redacted_tail(const std::string &value)
{
    if (value.empty()) return "empty";
    if (value.size() <= 4) return "****";
    return "****" + value.substr(value.size() - 4);
}

static std::string zoom_error_message(const QJsonObject &obj)
{
    const QString cmd = obj.value("cmd").toString();
    const QString msg = obj.value("msg").toString();
    const QString reason = obj.value("reason").toString();
    const QString name = obj.value("name").toString();
    const QString stage = obj.value("stage").toString();
    const QString auth_mode = obj.value("auth_mode").toString();
    const int code = obj.value("code").toInt(0);

    if (cmd == "auth_fail") {
        // Centralized error catalog (issue #89): map the Meeting SDK AuthResult
        // name onto a distinct, actionable operator message. The public-app-key
        // vs jwt mode changes how key/secret/jwt rejections are interpreted.
        const bool public_app_key_mode = auth_mode == "public_app_key";
        const zoom_join::ZoomJoinError category =
            zoom_join::classify_sdk_auth_result(name.toStdString(),
                                                public_app_key_mode);
        std::string out = zoom_join::join_error_guidance(category);
        // Keep the raw result code/name in the message for support bundles.
        out += " [";
        out += "sdk_auth_mode=" + (auth_mode.isEmpty()
                                       ? std::string("jwt")
                                       : auth_mode.toStdString());
        if (code != 0)
            out += " code=" + std::to_string(code);
        if (!name.isEmpty())
            out += " " + name.toStdString();
        if (!stage.isEmpty())
            out += " stage=" + stage.toStdString();
        out += "]";
        return out;
    }

    if (msg == "meeting_failed") {
        if (code == 63) {
            return "Zoom rejected the join: the signed-in Zoom user/ZAK was "
                   "sent, but this external meeting still requires the "
                   "Meeting SDK app/client ID to be published or approved by "
                   "Zoom for that host account. (63 "
                   "MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING)";
        }
        if (code == 505) {
            return "Zoom rejected the join: the on-behalf token is invalid. "
                   "(505 MEETING_FAIL_ON_BEHALF_TOKEN_INVALID)";
        }
        if (code == 506) {
            return "Zoom rejected the join: the on-behalf token does not match "
                   "this meeting. (506 MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING)";
        }
        if (code == 504) {
            return "Zoom rejected the join: this app cannot join anonymously. "
                   "(504 MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING)";
        }
        if (code == 500) {
            return "Zoom rejected the join: app privilege token error. "
                   "(500 MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR)";
        }
        std::string out = "Zoom meeting join failed";
        if (code != 0) out += " (" + std::to_string(code);
        if (!reason.isEmpty()) {
            out += code != 0 ? " " : " (";
            out += reason.toStdString();
        }
        if (code != 0 || !reason.isEmpty()) out += ")";
        return out;
    }

    if (!reason.isEmpty())
        return "Zoom engine error: " + reason.toStdString();
    if (!msg.isEmpty())
        return "Zoom engine error: " + msg.toStdString();
    return "Zoom engine error";
}

#if defined(WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
extern char **environ;
#endif

static std::string engine_executable_path()
{
#if defined(WIN32)
    HMODULE module = nullptr;
    char module_path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&engine_executable_path),
                           &module) &&
        GetModuleFileNameA(module, module_path, MAX_PATH) > 0) {
        std::string path(module_path);
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string module_dir = path.substr(0, slash + 1);
            std::string candidate = module_dir + "zoom-runtime\\ZoomObsEngine.exe";
            if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                return candidate;

            candidate = module_dir + "ZoomObsEngine.exe";
            if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                return candidate;
        }
    }

    char *obs_path = obs_module_file("ZoomObsEngine.exe");
    std::string candidate = obs_path ? obs_path : "ZoomObsEngine.exe";
    if (obs_path) bfree(obs_path);
    if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
        return candidate;
    return "ZoomObsEngine.exe";
#else
    // Resolve the engine next to the loaded plugin binary, mirroring the Windows
    // branch above. Returning a bare "ZoomObsEngine" is not sufficient: the
    // launcher uses posix_spawnp, which for a name containing no slash performs
    // a PATH search, and the engine ships inside the plugin bundle
    // (Contents/MacOS on macOS) which is never on PATH -- so the spawn failed
    // with ENOENT and the plugin could never start a meeting.
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&engine_executable_path), &info) &&
        info.dli_fname) {
        const std::string module_path(info.dli_fname);
        const size_t slash = module_path.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string module_dir = module_path.substr(0, slash + 1);
#if defined(__APPLE__)
            // Preferred macOS layout, and the only one where authentication can
            // work: the engine ships as an .app whose Contents/Frameworks holds
            // the Zoom SDK runtime, because the SDK loads its runtime bundles
            // through the main bundle rather than through rpath. A bare
            // executable beside the module still launches and still authenticates
            // *nothing* -- see engine/src/main-macos.mm. Checked first so a stale
            // loose binary from an older install cannot win.
            std::string candidate =
                module_dir + "ZoomObsEngine.app/Contents/MacOS/ZoomObsEngine";
            if (access(candidate.c_str(), X_OK) == 0) // flawfinder: ignore (own install paths, not a trust boundary)
                return candidate;

            candidate = module_dir + "zoom-runtime/ZoomObsEngine";
#else
            std::string candidate = module_dir + "zoom-runtime/ZoomObsEngine";
#endif
            if (access(candidate.c_str(), X_OK) == 0) // flawfinder: ignore (own install paths, not a trust boundary)
                return candidate;

            candidate = module_dir + "ZoomObsEngine";
            if (access(candidate.c_str(), X_OK) == 0) // flawfinder: ignore (own install paths, not a trust boundary)
                return candidate;
        }
    }

    char *obs_path = obs_module_file("ZoomObsEngine");
    if (obs_path) {
        const std::string candidate(obs_path);
        bfree(obs_path);
        if (access(candidate.c_str(), X_OK) == 0) // flawfinder: ignore (own install paths, not a trust boundary)
            return candidate;
    }
    // Last resort: let posix_spawnp search PATH, so a developer build with the
    // engine on PATH still works rather than failing outright.
    return "ZoomObsEngine";
#endif
}

#if defined(WIN32)
static std::string parent_directory(const std::string &path)
{
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}
#endif

static std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

ZoomEngineClient &ZoomEngineClient::instance()
{
    static ZoomEngineClient inst;
    return inst;
}

// THE READER THREAD MUST NOT DO MEDIA WORK (2026-08-17, full-1080p pressure
// test). handle_event() used to invoke the frame and audio callbacks inline,
// so every video event carried its whole SHM->OBS copy (~3MB at 1080p) on the
// pipe reader thread before the next line could be read. With Zoom granting
// 4x1920x1080 + 3x1600x900 feeds that thread fell behind its own pipe: audio
// events queued behind video events and arrived a measured 0.5-0.94s late,
// starving the audio ring's edge-triggered wakeups into the writer's 2.5s
// keepalive -- ~92% audio loss on every source, the same on-air signature as
// the ghost-writer wedge but with a healthy protocol underneath.
//
// The lanes fix the queuing discipline, not the unit cost. Media events are
// prompts, not payloads -- the video handler reads the NEWEST frame in the
// region, the audio handler drains EVERYTHING pending in the ring -- so the
// coalescing queue collapses any backlog to one dispatch per source
// (media-event-queue.h). Overloaded video degrades to fewer whole frames
// (latest-wins, the correct failure mode for an unbuffered source) instead of
// growing seconds of queue. Audio gets its OWN lane so its near-zero-cost
// drains never wait behind a video copy for a DIFFERENT source; same-source
// audio-vs-video ordering is guaranteed by the per-source callback gate both
// handlers already take, exactly as it was when one thread ran everything.
//
// Control events (roster, joined, left, debug) stay inline on the reader
// thread: they are cheap, and their relative order matters.
ZoomEngineClient::ZoomEngineClient()
{
    m_video_lane.thread = std::thread([this]() {
        m_video_lane.run([this](const std::string &uuid, const MediaEvent &e) {
            dispatch_media_event(true, uuid, e);
        });
    });
    m_audio_lane.thread = std::thread([this]() {
        m_audio_lane.run([this](const std::string &uuid, const MediaEvent &e) {
            dispatch_media_event(false, uuid, e);
        });
    });
}

ZoomEngineClient::~ZoomEngineClient()
{
    stop();
    m_video_lane.shutdown();
    m_audio_lane.shutdown();
}

void ZoomEngineClient::dispatch_media_event(bool is_frame,
                                            const std::string &uuid,
                                            const MediaEvent &event)
{
    SourceCallbacks callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_sources.find(uuid);
        // A miss is the designed outcome for an entry that outlived its
        // source (unregistered between enqueue and dispatch) -- drop it.
        if (it == m_sources.end()) return;
        callbacks = it->second;
    }
    if (is_frame) {
        if (callbacks.on_frame)
            callbacks.on_frame(event.p1, event.p2, event.p3, event.p4);
    } else {
        if (callbacks.on_audio)
            callbacks.on_audio(event.p1, event.p2, event.p3);
    }
}

// The fourth and last shape of one defect class: a plugin-side SHM read mapping
// that outlives the engine which named it.
//
// Two fixes already landed. The engine's generation counter moved into a
// process-wide table (src/shm-generation.h) so a rebuilt target keeps climbing
// instead of restarting at the legacy unsuffixed name, and the plugin releases
// its video mapping before every subscribe (src/zoom-source.cpp) and before
// every tile re-point (src/zoom-supersource.cpp). Both are scoped to ONE engine
// process.
//
// An engine restart escapes both. The new process starts with an empty table,
// so its first create for any region asks for generation 1 — the legacy
// unsuffixed name — and every mapping the plugin carried across the restart is
// sitting on exactly that name. On Windows the create then fails with
// ERROR_ACCESS_DENIED whenever the new region has to be larger than the one we
// are holding (src/shm-resubscribe.h has the operating-system rule in full).
//
// Video mostly survives that because resubscribe_all() re-subscribes through
// ZoomSource::subscribe(), which releases first. Audio does not: nothing on the
// restart path releases an audio mapping, and audio has no self-healing
// fallback — a failed ensure_shm() publishes no audio event
// (engine/src/engine-audio.cpp), so nothing ever prompts the plugin to reopen
// and that source stays silent for the rest of the session. The dedicated
// CoreVideo audio sources (src/zoom-participant-audio-source.cpp) release only
// in audio_destroy() — teardown, not recovery — so nothing lets go of their
// mapping while the source is alive.
//
// WHY THE TRIGGER IS THE RESTART AND NOT THE SUBSCRIBE. The obvious repair is
// to release audio on every subscribe path the way video does. That is wrong on
// cost and wrong on coverage. On cost: a bare subscribe makes the engine update
// its AudioTarget in place (EngineAudio::subscribe_if_needed) and recreate
// nothing, so a release there buys nothing and pays a remap on every
// active-speaker change, of which a show has thousands. On coverage: the
// subscribe paths are not the whole set of mapping holders — the tiles wall,
// the director preview and the dedicated audio sources each reach the engine by
// a different route, and each would need its own copy of the rule.
//
// One release per engine process, dispatched to every registered source, is
// cheaper than one release per subscribe AND covers more: video, share, audio,
// director preview and tiles together, on the single event that actually
// invalidates the whole name space.
//
// RELEASING IS ONLY HALF OF RECOVERY, and this function does only that half.
// It clears the way for the new engine's first create; something still has to
// ask the new engine for the feed. Video is asked for by
// ZoomOutputManager::resubscribe_all() on the reconnect path. The dedicated
// CoreVideo audio sources are not in that sweep — it iterates ZoomSource — so
// they clear their own stale subscription state inside their callback here and
// re-subscribe from the new engine's first roster
// (forget_subscription_for_new_engine() in
// src/zoom-participant-audio-source.cpp says why that is the trigger).
// Terminates every ZoomObsEngine process that exists before we launch a fresh
// one. Runs unconditionally on the launch path.
//
// THE DEFECT THIS EXISTS FOR (2026-08-17, live meeting, root-caused by
// controlled repro). An OBS exit or crash while the Zoom SDK is wedged leaves
// the engine process alive as an orphan -- still in the meeting, still
// receiving audio callbacks, still WRITING its shared-memory rings, with
// nobody reading its pipes. The next engine restarts its per-process
// generation counters, so it re-creates every region under the SAME name and
// CreateFileMapping silently hands it the ORPHAN'S section: two writers, one
// ring. The ghost's publishes set ShmAudioHeader::notify with no live pipe to
// deliver the event, which permanently suppresses the live engine's
// edge-triggered audio events -- every source degrades to the 2.5s keepalive
// (~92% audio loss; the exact live signature was reproduced on demand by
// attaching a synthetic 10Hz ghost writer to one healthy ring). It also holds
// the SDK singleton (the SDKERR_OTHER_SDK_INSTANCE_RUNNING init retry) and
// the named-pipe server instances.
//
// A wedged SDK ignores every polite signal -- that is what made it an orphan
// -- so hard termination is the only lever that works. Killing by image name
// is deliberate: any ZoomObsEngine that predates the process we are about to
// launch is stale by definition (the plugin only ever wants one), and the
// one we do want does not exist yet.
static void terminate_stale_engine_processes()
{
#if defined(WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Stale-engine sweep: process snapshot failed "
             "(error %lu) — launching anyway",
             GetLastError());
        return;
    }
    int killed = 0, refused = 0;
    // Explicit W variants: this is a UNICODE build, where the un-suffixed
    // names alias to them anyway and szExeFile is WCHAR[].
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"ZoomObsEngine.exe") != 0)
                continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                   pe.th32ProcessID);
            if (!h) {
                // Another session's engine running as another user, or gone
                // between snapshot and open. Count it: a refusal here is the
                // lead if shm_name_collision fires later anyway.
                ++refused;
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Stale-engine sweep: could not open "
                     "ZoomObsEngine pid=%lu (error %lu)",
                     pe.th32ProcessID, GetLastError());
                continue;
            }
            TerminateProcess(h, 1);
            // Bounded wait so the SDK singleton, pipe names and section names
            // are actually free before launch_engine() runs; an unwaited kill
            // can lose the race to its own replacement.
            WaitForSingleObject(h, 2000);
            CloseHandle(h);
            ++killed;
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Stale-engine sweep: terminated orphaned "
                 "ZoomObsEngine pid=%lu before launching a fresh engine",
                 pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (killed == 0 && refused == 0)
        blog(LOG_INFO, "[obs-zoom-plugin] Stale-engine sweep: none found");
#else
    // POSIX (mac port): same rationale, pkill by exact image name. -9 because
    // the process this hunts is by definition wedged past SIGTERM. Exit
    // status intentionally unchecked -- "nothing matched" and "killed one"
    // both leave the field clear for the launch that follows.
    (void)system("pkill -9 -x ZoomObsEngine 2>/dev/null");
#endif
}

void ZoomEngineClient::release_source_mappings_for_new_engine()
{
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (const auto &entry : m_sources)
            if (entry.second.on_new_engine_process)
                callbacks.push_back(entry.second.on_new_engine_process);
    }
    // m_mtx is released here — a callback takes its own source's lock and may
    // re-enter this client, and m_mtx is not recursive. Same rule as
    // update_roster_state_and_notify().
    for (const auto &cb : callbacks) cb();

    // Logged unconditionally, including the zero case. A restart that released
    // nothing is itself the evidence that no source was registered at the time,
    // which is what any future "it went silent after the engine restarted"
    // report needs to distinguish from "we released and it still broke".
    blog(LOG_INFO,
         "[obs-zoom-plugin] New ZoomObsEngine process: released shared-memory "
         "mappings for %zu registered source(s) before launching it",
         callbacks.size());
}

bool ZoomEngineClient::start(const std::string &jwt_token,
                             const std::string &public_app_key)
{
    if (m_running.load(std::memory_order_acquire)) return true;
    // Serialise the launch: see m_start_mtx in the header for the 9ms-apart
    // double-launch this closes. Deadlock note: nothing inside this body
    // calls start() and stop() never takes this lock, so the only wait here
    // is one racing starter finishing its launch.
    std::lock_guard<std::mutex> start_lock(m_start_mtx);
    // Re-check under the lock: if the racing caller we waited on succeeded,
    // the engine they launched is the engine we wanted.
    if (m_running.load(std::memory_order_acquire)) return true;
    if (jwt_token.empty() && public_app_key.empty()) {
        const std::string message =
            "Cannot start Zoom engine: no SDK auth credential is configured";
        set_last_error(message);
        blog(LOG_ERROR, "[obs-zoom-plugin] %s", message.c_str());
        return false;
    }
    m_last_jwt = public_app_key.empty() ? jwt_token : std::string();
    m_user_leaving.store(false, std::memory_order_release);
    m_authenticated.store(false, std::memory_order_release);
    m_media_active.store(false, std::memory_order_release);
    m_awaiting_admission.store(false, std::memory_order_release);
    // Task 5 fix round 2 (N2): a nomination-record reset used to live HERE,
    // before the joins immediately below -- reachable on the crash path
    // (monitor_loop() clears m_running without joining the reader, then
    // recovery calls start()), where a dead engine's already-queued
    // "nominate_done" could still be handled by the not-yet-joined reader and
    // commit AFTER a reset run this early, leaving a freshly restarted engine
    // with zero channels holding a stale confirmed plan. That bug was the
    // POSITION, not the existence of a reset here -- see the one after the
    // joins below, and stop_for_reconnect()'s own copy, for fix round 3 (N7)
    // restoring it correctly.
    // Join threads from any previous session (e.g. after a crash).
    if (m_reader.joinable())  m_reader.join();
    if (m_monitor.joinable()) m_monitor.join();

    // Task 5 fix round 3 (N7): a SECOND world-reset of the nomination record,
    // alongside stop_for_reconnect()'s (below). Both are needed, and both are
    // plain `= TalkbackNominationPlan{}`, so having both costs nothing:
    // stop_for_reconnect() is NOT on every path to a fresh start() --
    // monitor_loop() clears m_running directly (without calling it) when
    // recovery is DECLINED (policy disabled, auth failure, max attempts, no
    // stored session, or the m_user_leaving race), and so does
    // fail_after_init_retries_exhausted() (its own comment: clearing
    // m_running first is what "makes a subsequent start() work"). On both,
    // the operator's next action is a manual dock Join, which calls start()
    // directly -- no stop()/stop_for_reconnect() in between -- and round 2's
    // fix left exactly that path holding the dead engine's confirmed plan
    // again (N7, the same F2/N1 symptom on a third trigger). Placed AFTER the
    // joins above, unlike round 1's original mistake at this same call site:
    // the previous session's reader thread owns this field and can still be
    // inside handle_event() committing a queued "nominate_done" until it is
    // joined.
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        talkback_nomination_reset(m_talkback_nomination_status);
        m_talkback_nomination_pending = TalkbackNominationPending{};
        talkback_presence_reset(m_talkback_channel_presence);
    }

    // Fresh engine session: no init retry is owed. This must come AFTER the
    // joins above — the previous session's reader thread owns these fields and
    // can still be inside handle_event() until it is joined.
    m_init_retry_due_ms.store(0, std::memory_order_release);
    m_init_teardown_pending.store(false, std::memory_order_release);
    m_init_retry_attempts = 0;
    m_init_retry_waited_ms = 0;

    // Everything the plugin still has mapped is standing on a name the next
    // engine process is about to reuse, so drop it all now — BEFORE that
    // process exists.
    //
    // Before the new engine's first create is necessary but not sufficient.
    // A subscribe reaches the engine through write_json(), which checks only
    // the pipe; the m_running gate is read earlier, by the caller. A thread
    // that passed that gate while the old engine was alive and then parked can
    // resume any time after connect_ipc() installs the new pipe, and its stale
    // subscribe would be the new engine's first prompt to create a region —
    // ahead of a release still queued behind it. Releasing before launch_engine()
    // puts the release ahead of everything the new process can be told, so that
    // race has no window left to run in rather than a small one.
    //
    // Nothing here needs the new engine: every on_new_engine_process callback
    // unmaps and does no more (that is a documented precondition of the
    // callback), so it is safe this early. And if the launch below fails, the
    // release cost nothing — the mappings belonged to a process that is already
    // dead, and the regions are reopened from the first frame or audio event
    // whenever an engine does come up.
    release_source_mappings_for_new_engine();

    // AFTER the release (our own mappings on those names are gone), BEFORE the
    // launch (so the SDK singleton, pipe names and section names are free for
    // the process we are about to create). See the function's comment for the
    // ghost-writer defect this closes.
    terminate_stale_engine_processes();

    if (!launch_engine() || !connect_ipc()) {
        disconnect_ipc();
        // Engine may have been launched before IPC connection failed — kill it.
#if defined(WIN32)
        if (m_process) {
            TerminateProcess(static_cast<HANDLE>(m_process), 1);
            WaitForSingleObject(static_cast<HANDLE>(m_process), 3000);
            CloseHandle(static_cast<HANDLE>(m_process));
            m_process = nullptr;
        }
#else
        if (m_pid > 0) {
            kill(m_pid, SIGTERM);
            waitpid(m_pid, nullptr, 0);
            m_pid = -1;
        }
#endif
        return false;
    }

    // Seed the heartbeat clock so a freshly connected engine isn't immediately
    // considered stale before its first line arrives.
    m_last_rx_ms.store(os_gettime_ns() / 1000000ULL, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_reader  = std::thread([this]() { reader_loop(); });
    m_monitor = std::thread([this]() { monitor_loop(); });
    std::string init_json;
    if (!public_app_key.empty()) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Zoom engine init auth=public_app_key public_app_key_tail=%s jwt_present=0",
             redacted_tail(public_app_key).c_str());
        init_json = R"({"cmd":"init","public_app_key":")" +
                    json_escape(public_app_key) + "\"}";
    } else {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Zoom engine init auth=jwt public_app_key_tail=empty jwt_present=%d",
             jwt_token.empty() ? 0 : 1);
        init_json = R"({"cmd":"init","jwt":")" + json_escape(jwt_token) + "\"}";
    }
    {
        // Kept so monitor_loop() can replay it verbatim if the engine's first
        // InitSDK loses the race against an orphaned engine holding the SDK.
        std::lock_guard<std::mutex> lk(m_mtx);
        m_init_payload = init_json;
    }
    write_json(init_json);
    return true;
}

void ZoomEngineClient::stop()
{
    m_user_leaving.store(true, std::memory_order_release);
    // Cancel any pending reconnect BEFORE tearing down the engine so a
    // queued execute_retry on the UI thread cannot resurrect us.
    ZoomReconnectManager::instance().cancel();
    ZoomReconnectManager::instance().clear_session();
    stop_for_reconnect();
}

void ZoomEngineClient::stop_for_reconnect()
{
    if (m_running.exchange(false, std::memory_order_acq_rel)) {
        write_json(R"({"cmd":"quit"})");
        disconnect_ipc();
    }
    // Always join both threads — safe even if they already exited.
    if (m_reader.joinable())  m_reader.join();
    if (m_monitor.joinable()) m_monitor.join(); // also reaps the child process
    // Drop any pending SDK-init retry. The monitor thread owned the wait and is
    // joined by now, so nothing can fire it; clearing the payload also drops the
    // SDK credential it carries. start() repopulates both.
    m_init_retry_due_ms.store(0, std::memory_order_release);
    m_init_teardown_pending.store(false, std::memory_order_release);
    m_init_retry_attempts = 0;
    m_init_retry_waited_ms = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_init_payload.clear();
    }
    m_authenticated.store(false, std::memory_order_release);
    m_media_active.store(false, std::memory_order_release);
    m_awaiting_admission.store(false, std::memory_order_release);
    m_state.store(MeetingState::Idle, std::memory_order_release);
    // Task 5 fix round 2 (N2/N4): the third world-reset point, alongside the
    // per-session fields above -- covers an engine restart (crash recovery's
    // execute_retry() calls this before the next start()) AND the window
    // between an operator stop()/leave and the next start(), where
    // talkback_status would otherwise keep advertising a plan whose engine
    // process no longer exists. Placed AFTER the joins above (not before,
    // where round 1 had the equivalent block in start() -- see the comment
    // there): the previous session's reader thread owns this field and can
    // still be inside handle_event() committing a queued "nominate_done"
    // until it is joined. Fix round 3 (N7): start() ALSO resets this, after
    // its own joins -- not every path to a fresh start() passes through here
    // first (monitor_loop() declining recovery, or
    // fail_after_init_retries_exhausted(), both clear m_running directly),
    // and a manual dock Join calls start() with nothing in between. Both
    // resets are idempotent plain `= TalkbackNominationPlan{}`, so keeping
    // both costs nothing and covers every trigger.
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_raw_media_error.clear();
        m_privilege_notice.clear();
        m_media_failures.reset();
        talkback_nomination_reset(m_talkback_nomination_status);
        m_talkback_nomination_pending = TalkbackNominationPending{};
        talkback_presence_reset(m_talkback_channel_presence);
    }
}

void ZoomEngineClient::monitor_loop()
{
    // Poll until the engine process exits OR it stops responding (heartbeat
    // timeout). A hung-but-alive engine (process up, pipe silent) would
    // otherwise block reader_loop on ipc_read_line() forever and never recover.
    constexpr uint64_t kHeartbeatTimeoutMs = 10000;
    int exit_code = 0;
    bool heartbeat_timeout = false;

    while (m_running.load(std::memory_order_acquire)) {
#if defined(WIN32)
        if (m_process) {
            DWORD waited = WaitForSingleObject(static_cast<HANDLE>(m_process), 1000);
            if (waited == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(static_cast<HANDLE>(m_process), &code);
                exit_code = static_cast<int>(code);
                CloseHandle(static_cast<HANDLE>(m_process));
                m_process = nullptr;
                break;
            }
        } else {
            break;
        }
#else
        if (m_pid > 0) {
            int status = 0;
            pid_t r = waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid) {
                exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                m_pid = -1;
                break;
            }
            if (r < 0) { // process already reaped / error
                m_pid = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        } else {
            break;
        }
#endif
        // Engine is still alive — check that it is still talking to us. Only
        // enforce the timeout once we're connected/in a meeting, where a
        // steady heartbeat is expected.
        const MeetingState st = m_state.load(std::memory_order_acquire);
        if (m_running.load(std::memory_order_acquire) &&
            (st == MeetingState::InMeeting || st == MeetingState::Joining)) {
            const uint64_t now = os_gettime_ns() / 1000000ULL;
            const uint64_t last = m_last_rx_ms.load(std::memory_order_acquire);
            if (ipc_heartbeat_expired(now, last, kHeartbeatTimeoutMs)) {
                heartbeat_timeout = true;
                break;
            }
        }

        // The reader thread ran out of SDK-init retries and handed us the
        // teardown, because it cannot stop the engine from the thread stop()
        // joins. This exits monitor_loop() without triggering recovery: an
        // auth failure is permanent, retries are already spent.
        if (m_init_teardown_pending.load(std::memory_order_acquire)) {
            fail_after_init_retries_exhausted();
            return;
        }

        // Deliver a due SDK-init retry (see the m_init_retry_due_ms comment in
        // the header for why the wait lives on this thread). The engine is
        // still up and idle after a failed InitSDK — it logged auth_fail and
        // went back to reading commands — so replaying the init command is all
        // that is needed; no relaunch. This loop's ~1s tick is the resolution
        // of the wait, which is fine for a 2s/4s/8s schedule.
        const uint64_t due = m_init_retry_due_ms.load(std::memory_order_acquire);
        if (due != 0 && (os_gettime_ns() / 1000000ULL) >= due &&
            m_running.load(std::memory_order_acquire)) {
            uint64_t expected = due;
            if (m_init_retry_due_ms.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel)) {
                std::string payload;
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    payload = m_init_payload;
                }
                // write_json() takes m_mtx, so it is called with the lock above
                // already released.
                if (!payload.empty()) {
                    blog(LOG_INFO,
                         "[obs-zoom-plugin] Retrying Zoom SDK init (waiting out another "
                         "SDK instance)");
                    write_json(payload);
                }
            }
        }
    }

    // If m_running is still true the engine exited (or went silent) without
    // being asked to.
    if (m_running.exchange(false, std::memory_order_acq_rel)) {
        // ...unless we broke out of the loop while an exhausted-init teardown
        // was in flight (the engine exited or went silent in that window). That
        // failure still wins: it explains what actually happened, and it is not
        // a crash to reconnect from.
        if (m_init_teardown_pending.load(std::memory_order_acquire)) {
            fail_after_init_retries_exhausted();
            return;
        }
        if (heartbeat_timeout) {
            blog(LOG_ERROR,
                 "[obs-zoom-plugin] ZoomObsEngine stopped responding (no IPC for >%llums)",
                 static_cast<unsigned long long>(kHeartbeatTimeoutMs));
            set_last_error("Zoom engine stopped responding");
            // The process is hung but still alive — terminate and reap it so the
            // recovery path starts from a clean slate, mirroring the crash case.
#if defined(WIN32)
            if (m_process) {
                TerminateProcess(static_cast<HANDLE>(m_process), 1);
                WaitForSingleObject(static_cast<HANDLE>(m_process), 3000);
                CloseHandle(static_cast<HANDLE>(m_process));
                m_process = nullptr;
            }
#else
            if (m_pid > 0) {
                kill(m_pid, SIGKILL);
                waitpid(m_pid, nullptr, 0);
                m_pid = -1;
            }
#endif
        } else {
            blog(LOG_ERROR,
                 "[obs-zoom-plugin] ZoomObsEngine exited unexpectedly (code %d)",
                 exit_code);
        }
        disconnect_ipc(); // unblocks reader_loop so it exits cleanly

        // If the user is in the middle of leaving / stopping, don't try to recover —
        // we lost a race against stop() but the user's intent is clear.
        //
        // THE DEFECT THIS LOG LINE EXISTS FOR (2026-08-21, live incident). A
        // crash landing here is otherwise completely silent: the line above
        // already logs the crash itself, but the decision to skip recovery
        // produces nothing at all -- every other way trigger() declines to
        // reconnect (policy disabled, auth failure, max attempts, no stored
        // session) logs its own reason; this was the one silent exit. Live,
        // that meant a meeting that had already self-healed from one earlier
        // crash (full "Scheduling reconnect... succeeded" trail in the log)
        // hit a second crash that produced only the exit-code line and then
        // nothing -- no evidence for whether m_user_leaving was set by a
        // genuine Leave/Stop, a stale flag from an earlier one, or the dock's
        // 120s join-timeout watchdog (zoom-dock.cpp) auto-leaving a stuck
        // reconnect, and no way to tell which after the fact.
        if (m_user_leaving.load(std::memory_order_acquire)) {
            blog(LOG_INFO,
                 "[obs-zoom-plugin] Engine exit not recovered: user_leaving "
                 "was already set (an explicit Leave/Stop, or the dock's "
                 "join-timeout watchdog, ran before or during this exit)");
            m_state.store(MeetingState::Idle, std::memory_order_release);
            return;
        }

        RecoveryReason reason = RecoveryReason::EngineCrash;
        if (!heartbeat_timeout) {
            if (exit_code == 2) reason = RecoveryReason::AuthFailure;
            else if (exit_code == 3) reason = RecoveryReason::SdkError;
            else if (exit_code == 4) reason = RecoveryReason::LicenseError;
        }

        m_state.store(MeetingState::Recovering, std::memory_order_release);
        ZoomReconnectManager::instance().trigger(reason);
    }
}

bool ZoomEngineClient::join(const std::string &meeting_id,
                            const std::string &passcode,
                            const std::string &display_name,
                            MeetingKind kind,
                            const ZoomJoinAuthTokens &tokens)
{
    if (!m_running.load(std::memory_order_acquire)) return false;
    if (meeting_id.empty()) return false;
    m_state.store(MeetingState::Joining, std::memory_order_release);
    clear_last_error();
    // Always keep session params up to date for recovery.
    ZoomReconnectManager::instance().store_session(
        m_last_jwt, meeting_id, passcode, display_name, kind, tokens);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // An explicit join replaces the session; room-level joined reports do not.
        m_raw_media_error.clear();
        m_privilege_notice.clear();
        m_media_failures.reset();
        m_join_pending = true;
        m_pending_meeting_id = meeting_id;
        m_pending_passcode = passcode;
        m_pending_display_name = display_name;
        m_pending_tokens = tokens;
        m_pending_kind = kind;
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom join queued: meeting_id=%s authenticated=%d",
             meeting_id.c_str(),
             m_authenticated.load(std::memory_order_acquire) ? 1 : 0);
        if (m_authenticated.load(std::memory_order_acquire))
            send_join_locked();
    }
    return true;
}

void ZoomEngineClient::subscribe_spotlight(const std::string &source_uuid, uint32_t slot)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Engine-selected participant; failure membership still records the
        // actual participant and only that participant's read can recover it.
        m_media_failures.assign(source_uuid, 0);
    }
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","mode":"spotlight","slot":)" + std::to_string(slot) + "}");
}

void ZoomEngineClient::subscribe_screenshare(const std::string &source_uuid)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Engine-selected participant; failure membership still records the
        // actual participant and only that participant's read can recover it.
        m_media_failures.assign(source_uuid, 0);
    }
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","mode":"screenshare"})");
}

void ZoomEngineClient::leave()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    m_user_leaving.store(true, std::memory_order_release);
    ZoomReconnectManager::instance().cancel(); // suppress any in-progress recovery
    // Explicit user leave is a deliberate end of participation: drop the stored
    // recovery session so sensitive join credentials (ZAK / on-behalf /
    // app-privilege tokens) are wiped from memory rather than lingering until
    // the next join() or stop(). Mirrors stop(); a subsequent rejoin re-stores
    // a fresh session via join().
    ZoomReconnectManager::instance().clear_session();
    m_state.store(MeetingState::Leaving, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_join_pending = false;
    }
    write_json(R"({"cmd":"leave"})");
}

void ZoomEngineClient::start_media()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"start_media"})");
}

void ZoomEngineClient::stop_media()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"stop_media"})");
    m_media_active.store(false, std::memory_order_release);
}

void ZoomEngineClient::talkback_probe(const std::string &participant_name)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_probe","participant":")" +
               json_escape(participant_name) + "\"}");
}

void ZoomEngineClient::talkback_start(const std::string &target)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    // F2 review-round fix: reset the engine-confirmed session state at the
    // moment a NEW session is requested, so TalkbackController::evaluate()'s
    // grace period (see there) starts from a known "not yet answered"
    // baseline (live=false, reason empty) instead of a stale live/reason
    // left over from a PREVIOUS key's session -- without this, a key closed
    // for cause and then immediately reopened would inherit the old
    // session's failure reason and get closed again before the new
    // session's own CreateChannel round-trip ever had a chance to answer.
    // Scoped so the lock is released before write_json() re-acquires it
    // (m_mtx is not recursive).
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_talkback_session_status = TalkbackSessionStatus{};
    }
    // Task 5: "target", not "participant" -- see this method's header
    // comment. main.cpp still accepts the old field as a fallback, but this
    // plugin only ever sends the new one.
    write_json(R"({"cmd":"talkback_start","target":")" +
               json_escape(target) + "\"}");
}

void ZoomEngineClient::talkback_nominate(const std::vector<std::string> &nominees)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    // Task 5 fix round 1 (F4): dedupe here, matching talkback_plan()'s own
    // collapsing of duplicate nominees (src/talkback-plan.h) -- the engine
    // reports `channels` post-dedup, so recording the raw list here would
    // inflate the plugin's own has_private_channel count against it.
    const std::vector<std::string> deduped = talkback_dedup_preserve_order(nominees);
    // Task 5 fix round 1 (F1): stage this attempt in the PENDING record only
    // -- m_talkback_nomination_status (the CONFIRMED plan) must not move
    // until the engine actually accepts it. See src/talkback-nomination.h's
    // header comment: a refused nomination leaves the engine's standing
    // channel set untouched, so overwriting the confirmed plan at send time
    // (the old behaviour) falsely refused a key on a still-standing channel
    // whenever a re-nomination was refused. Scoped so the lock is released
    // before write_json() re-acquires it (m_mtx is not recursive), same
    // discipline as talkback_start() above.
    // Task 5 final review (C1, CRITICAL): stamp this send with an attempt id
    // and stage under it. Without it, a re-nomination sent while an earlier
    // ladder was still provisioning wiped the earlier attempt's staging, and
    // that ladder's own nominate_done committed THIS attempt's nominee list
    // against the earlier ladder's channels -- see src/talkback-nomination.h.
    uint32_t attempt = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        attempt = ++m_talkback_nominate_attempt;
        talkback_nomination_begin(m_talkback_nomination_pending, deduped, attempt);
        // Milestone 7: the standing channel set is about to be REPLACED
        // (nominate()'s replace path destroys it before planning), so every
        // presence observation this record holds is about channels that are
        // going away. Clearing to Unknown fails soft -- the grid reads
        // "ready" until the new ladder's own invites report -- which is the
        // right direction for a display-only record. See
        // talkback_presence_reset().
        talkback_presence_reset(m_talkback_channel_presence);
    }
    // Task 5 fix round 1 (F5, documented not fixed): json_escape() below
    // escapes '\n'/'\r'/'\t' as two-character sequences ("\\n" etc.), but the
    // engine's line-oriented parser (json_str/json_str_array,
    // engine/src/main.cpp) is not a real JSON decoder -- it only knows
    // "a backslash means take the NEXT BYTE literally", so it decodes "\\n"
    // to a literal 'n' character, not a newline. '"' and '\\' happen to
    // round-trip correctly under that rule (escaped-quote and
    // escaped-backslash both collapse to the original byte), but a nominee
    // display name containing an actual control character would not: the
    // engine would plan around a different string than `requested` holds,
    // silently desyncing uncovered_private/has_private_channel and letting a
    // key by the real name be locally allowed then engine-refused (or vice
    // versa). Not fixed here: the decoder is shared by every P2E command,
    // not something to change opportunistically for one caller, and display
    // names containing control characters are exceedingly rare in practice --
    // the failure mode is a refusal or a coverage mismatch, not data
    // corruption. A real fix means teaching the engine's decoder actual JSON
    // escape semantics.
    // KEY ORDER IS LOAD-BEARING: "attempt" comes BEFORE "nominees". The
    // engine's json_uint() (engine/src/main.cpp) is a first-match scan for
    // "\"attempt\":", not a real JSON parser, so a nominee display name is
    // the only thing that could shadow this field -- and only if it reached
    // the wire with an UNESCAPED quote before it, which json_escape() below
    // prevents (an escaped one reads as \" and does not match the needle).
    // Emitting the id ahead of the participant-controlled array means the
    // right value is found first even if that ever stops being true. Same
    // reasoning json_str_array()'s own header comment gives for treating this
    // decoder's limits as a constraint to design around rather than an
    // assumption to rely on.
    std::string json = R"({"cmd":"talkback_nominate","attempt":)" +
                       std::to_string(attempt) + R"(,"nominees":[)";
    for (std::size_t i = 0; i < deduped.size(); ++i) {
        if (i != 0) json += ",";
        json += "\"" + json_escape(deduped[i]) + "\"";
    }
    json += "]}";
    write_json(json);
}

void ZoomEngineClient::talkback_stop()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_stop"})");
}

void ZoomEngineClient::talkback_open(const std::string &region, uint32_t rate,
                                     uint16_t channels)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_open","region":")" + json_escape(region) +
               R"(","rate":)" + std::to_string(rate) +
               R"(,"channels":)" + std::to_string(channels) + "}");
}

void ZoomEngineClient::talkback_audio()
{
    // Fires once per empty->non-empty ring edge -- see the call site in
    // talkback-tap.cpp's on_audio() for why sending on every buffer instead
    // would recreate the message-storm shape this codebase already has a
    // live incident about.
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_audio"})");
}

void ZoomEngineClient::talkback_close()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_close"})");
}

void ZoomEngineClient::subscribe(const std::string &source_uuid,
                                 uint32_t participant_id,
                                 bool isolate_audio,
                                 bool audience_audio,
                                 VideoResolution video_resolution,
                                 bool video_only)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_media_failures.assign(source_uuid, participant_id);
    }
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","participant_id":)" + std::to_string(participant_id) +
        R"(,"resolution":)" + std::to_string(static_cast<int>(video_resolution)) +
        R"(,"isolate_audio":)" + std::string(isolate_audio ? "true" : "false") +
        R"(,"audience_audio":)" + std::string(audience_audio ? "true" : "false") +
        R"(,"video_only":)" + std::string(video_only ? "true" : "false") +
        "}");
}

bool ZoomEngineClient::subscribe_audio(const std::string &source_uuid,
                                       uint32_t participant_id,
                                       bool isolate_audio,
                                       bool audience_audio)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty())
        return false;
    return write_json(R"({"cmd":"subscribe_audio","source_uuid":")" + json_escape(source_uuid) +
        R"(","participant_id":)" + std::to_string(participant_id) +
        R"(,"isolate_audio":)" + std::string(isolate_audio ? "true" : "false") +
        R"(,"audience_audio":)" + std::string(audience_audio ? "true" : "false") +
        "}");
}

void ZoomEngineClient::unsubscribe(const std::string &source_uuid)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_media_failures.remove(source_uuid);
    }
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    write_json(R"({"cmd":"unsubscribe","source_uuid":")" + json_escape(source_uuid) + "\"}");
}

void ZoomEngineClient::register_source(const std::string &source_uuid,
                                       SourceCallbacks callbacks)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sources[source_uuid] = std::move(callbacks);
}

void ZoomEngineClient::unregister_source(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sources.erase(source_uuid);
    m_media_failures.remove(source_uuid);
}

bool ZoomEngineClient::source_media_failed(const std::string &uuid) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_media_failures.failed(uuid);
}

uint64_t ZoomEngineClient::media_delivery_ticket(const std::string &uuid, uint32_t participant) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_media_failures.ticket(uuid, participant);
}

void ZoomEngineClient::acknowledge_media_delivery(const std::string &uuid, uint32_t participant, uint64_t ticket)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_media_failures.delivered(uuid, participant, ticket);
    // Dock polling observes recovery. Never call UI/source callbacks while a
    // source's frame lock may be held, or queue one UI task per video frame.
}

bool ZoomEngineClient::launch_engine()
{
#if defined(WIN32)
    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    const std::string engine_path = engine_executable_path();
    const std::string engine_dir = parent_directory(engine_path);
    std::string command = "\"" + engine_path + "\"";
    blog(LOG_INFO, "[obs-zoom-plugin] Launching ZoomObsEngine: %s",
         engine_path.c_str());

    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr,
                        engine_dir.empty() ? nullptr : engine_dir.c_str(),
                        &si, &pi)) {
        const DWORD code = GetLastError();
        const std::string message =
            "Failed to launch ZoomObsEngine: Windows error " + std::to_string(code);
        set_last_error(message);
        blog(LOG_ERROR, "[obs-zoom-plugin] %s", message.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    return true;
#else
    const std::string path = engine_executable_path();
    blog(LOG_INFO, "[obs-zoom-plugin] Launching ZoomObsEngine: %s", path.c_str());
    char *const argv[] = {const_cast<char *>(path.c_str()), nullptr};
    pid_t pid = -1;
    // posix_spawnp returns the error number directly; it does not set errno.
    const int rc = posix_spawnp(&pid, path.c_str(), nullptr, nullptr, argv, environ);
    if (rc != 0) {
        // Name the path and the reason: "failed to launch" alone gives no way to
        // tell a missing engine from a non-executable one.
        const std::string message = "Failed to launch ZoomObsEngine at '" + path +
                                    "': " + strerror(rc);
        set_last_error(message);
        blog(LOG_ERROR, "[obs-zoom-plugin] %s", message.c_str());
        return false;
    }
    m_pid = pid;
    return true;
#endif
}

bool ZoomEngineClient::connect_ipc()
{
#if defined(WIN32)
    constexpr int kAttempts = 300;
    for (int i = 0; i < kAttempts; ++i) {
        m_p2e = CreateFileA(PIPE_P2E, GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        m_e2p = CreateFileA(PIPE_E2P, GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (m_p2e != kIpcInvalidFd && m_e2p != kIpcInvalidFd) return true;
        disconnect_ipc();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    blog(LOG_ERROR,
         "[obs-zoom-plugin] Timed out connecting to ZoomObsEngine IPC pipes. "
         "The engine may have exited during startup; check that Zoom SDK runtime "
         "DLLs are beside ZoomObsEngine.exe.");
    set_last_error("Timed out connecting to ZoomObsEngine. Check that the full "
                   "Zoom SDK runtime DLLs are beside ZoomObsEngine.exe.");
    return false;
#else
    auto connect_one = [](const char *path) -> int {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    };
    constexpr int kAttempts = 300;
    for (int i = 0; i < kAttempts; ++i) {
        m_p2e = connect_one(SOCK_P2E);
        m_e2p = connect_one(SOCK_E2P);
        if (m_p2e != kIpcInvalidFd && m_e2p != kIpcInvalidFd) return true;
        disconnect_ipc();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    blog(LOG_ERROR,
         "[obs-zoom-plugin] Timed out connecting to ZoomObsEngine IPC sockets. "
         "The engine may have exited during startup.");
    set_last_error("Timed out connecting to ZoomObsEngine IPC sockets. The engine "
                   "may have exited during startup.");
    return false;
#endif
}

void ZoomEngineClient::disconnect_ipc()
{
    // Hold m_mtx so that write_json() cannot use a handle after CloseHandle/close().
    std::lock_guard<std::mutex> lk(m_mtx);
#if defined(WIN32)
    if (m_p2e != kIpcInvalidFd) { CloseHandle(m_p2e); m_p2e = kIpcInvalidFd; }
    if (m_e2p != kIpcInvalidFd) { CloseHandle(m_e2p); m_e2p = kIpcInvalidFd; }
#else
    if (m_p2e != kIpcInvalidFd) { close(m_p2e); m_p2e = kIpcInvalidFd; }
    if (m_e2p != kIpcInvalidFd) { close(m_e2p); m_e2p = kIpcInvalidFd; }
#endif
}

void ZoomEngineClient::set_last_error(const std::string &message)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_last_error = message;
}

std::string ZoomEngineClient::talkback_probe_status() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_talkback_probe_status;
}

ZoomEngineClient::TalkbackSessionStatus ZoomEngineClient::talkback_session_status() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_talkback_session_status;
}

ZoomEngineClient::TalkbackNominationStatus ZoomEngineClient::talkback_nomination_status() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_talkback_nomination_status;
}

TalkbackChannelPresence ZoomEngineClient::talkback_channel_presence() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_talkback_channel_presence;
}

void ZoomEngineClient::fail_after_init_retries_exhausted()
{
    // Monitor thread only.
    const std::string message =
        sdk_init_other_instance_message(m_init_retry_waited_ms);

    // Clearing m_running FIRST is deliberate. It is what makes a subsequent
    // start() work: that call joins this thread (so it waits for the teardown
    // below to finish) and then relaunches. If we cleared it last, a request
    // arriving mid-teardown would early-return and be silently swallowed.
    m_running.store(false, std::memory_order_release);
    write_json(R"({"cmd":"quit"})");
#if defined(WIN32)
    if (m_process) {
        if (WaitForSingleObject(static_cast<HANDLE>(m_process), 3000) !=
            WAIT_OBJECT_0) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] ZoomObsEngine did not exit on quit after a failed "
                 "SDK init; terminating it so the next request can relaunch");
            TerminateProcess(static_cast<HANDLE>(m_process), 1);
            WaitForSingleObject(static_cast<HANDLE>(m_process), 3000);
        }
        CloseHandle(static_cast<HANDLE>(m_process));
        m_process = nullptr;
    }
#else
    if (m_pid > 0) {
        kill(m_pid, SIGTERM);
        waitpid(m_pid, nullptr, 0);
        m_pid = -1;
    }
#endif
    disconnect_ipc(); // unblocks reader_loop so it exits cleanly

    m_authenticated.store(false, std::memory_order_release);
    m_media_active.store(false, std::memory_order_release);
    m_awaiting_admission.store(false, std::memory_order_release);
    m_init_retry_due_ms.store(0, std::memory_order_release);
    // m_init_retry_attempts / m_init_retry_waited_ms are deliberately NOT reset
    // here: disconnect_ipc() above may still let the reader thread run one more
    // handle_event() on a buffered line before it observes m_running == false,
    // and that thread owns these two non-atomic fields. Resetting them here would
    // race with it. start() and stop_for_reconnect() both join the reader thread
    // before resetting these fields, so the reset still always happens before
    // the next session can use them.
    m_init_teardown_pending.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_init_payload.clear();
    }

    // Surface the failure only AFTER the teardown: a failing write_json() or a
    // broken pipe on the way out overwrites m_last_error with "Lost connection
    // to Zoom engine", which would bury the message that actually explains
    // this. Same permanent disposition as any other auth_fail — only the text
    // the operator reads is different.
    blog(LOG_ERROR, "[obs-zoom-plugin] %s", message.c_str());
    set_error_and_notify(message);
    m_state.store(MeetingState::Failed, std::memory_order_release);
    ZoomReconnectManager::instance().on_join_failed(true);
}

void ZoomEngineClient::set_error_and_notify(const std::string &message)
{
    std::vector<ErrorCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_last_error = message;
        for (const auto &entry : m_error_callbacks)
            if (entry.second) callbacks.push_back(entry.second);
    }
    // m_mtx is released here — a callback may call back into this client.
    for (const auto &cb : callbacks) cb(message);
}

void ZoomEngineClient::set_privilege_notice_and_notify(const std::string &message)
{
    std::vector<NoticeCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_privilege_notice == message) return;
        m_privilege_notice = message;
        for (const auto &entry : m_notice_callbacks)
            if (entry.second) callbacks.push_back(entry.second);
    }
    // m_mtx is released here — a callback may call back into this client, same
    // reason as set_error_and_notify() above.
    for (const auto &cb : callbacks) cb(message);
}

void ZoomEngineClient::clear_privilege_notice_and_notify()
{
    std::vector<NoticeCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_privilege_notice.empty())
            return; // nothing pending -- most raw_media_ready reports land here.
        m_privilege_notice.clear();
        for (const auto &entry : m_notice_callbacks)
            if (entry.second) callbacks.push_back(entry.second);
    }
    for (const auto &cb : callbacks) cb(std::string());
}

void ZoomEngineClient::reader_loop()
{
    std::string line;
    while (m_running.load(std::memory_order_acquire) &&
           ipc_read_line(m_e2p, line)) {
        handle_event(line);
    }
}

void ZoomEngineClient::handle_event(const std::string &line)
{
    // Record receipt of any line so monitor_loop() can detect a silent engine.
    m_last_rx_ms.store(os_gettime_ns() / 1000000ULL, std::memory_order_release);

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line));
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString cmd = obj.value("cmd").toString();

    if (cmd == "ping") {
        // Heartbeat from the engine — the timestamp update above is all we need.
        return;
    }
    if (cmd == "ready") {
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom engine ready");
        return;
    }
    if (cmd == "auth_ok") {
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom engine authenticated");
        m_authenticated.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_join_pending)
                send_join_locked();
        }
        return;
    }
    if (cmd == "debug") {
        const QString stage = obj.value("stage").toString();
        if (cv_zoom_verbose_logging() || !is_high_frequency_stage(stage))
            blog(LOG_INFO, "[obs-zoom-plugin] Zoom engine debug: %s",
                 line.c_str());
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            DebugEvent event;
            event.timestamp_ms = os_gettime_ns() / 1000000ULL;
            event.stage = stage.toStdString();
            event.source_uuid = obj.value("source_uuid").toString().toStdString();
            event.participant_id =
                static_cast<uint32_t>(obj.value("participant_id").toInt(0));
            event.message = line;
            m_debug_events.push_back(std::move(event));
            while (m_debug_events.size() > 300)
                m_debug_events.pop_front();
        }
        if (stage == "raw_media_ready") {
            m_media_active.store(true, std::memory_order_release);
            // raw_media_ready is the engine event that means the record-
            // privilege handshake (if one was in progress) just succeeded --
            // see src/zoom-privilege-notice.h. A notice that never clears is
            // its own defect, so clear it on every successful start, not just
            // ones that followed a notice.
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_raw_media_error.clear();
            }
            clear_privilege_notice_and_notify();
        } else if (stage == "raw_media_state") {
            // Session readiness is separate from per-source frame health.
            const auto media_state = obj.value("state").toString().toStdString();
            if (media_state != "active")
                m_media_active.store(false, std::memory_order_release);
            const auto notice = zoom_raw_media_state_notice(media_state,
                obj.value("reason").toString().toStdString());
            if (!notice.empty()) set_privilege_notice_and_notify(notice);
            else if (media_state == "stopped") clear_privilege_notice_and_notify();
        } else if (stage == "raw_media_stopped") {
            m_media_active.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_raw_media_error.clear();
            m_media_failures.stop();
        }
        return;
    }
    if (cmd == "talkback_probe") {
        // Milestone 1's entire deliverable is these stage reports reaching
        // the operator -- verbatim, no filtering/summarising/pretty-printing,
        // because a stage that doesn't reach the log may as well not have
        // been reported.
        blog(LOG_INFO, "[obs-zoom-plugin] talkback_probe: %s", line.c_str());
        // Also stash the raw line for the dock's status label so the operator
        // isn't required to tail the log to use the probe button. Lock scope
        // is kept to the copy alone -- never held across the blog() above or
        // any Qt call the dock might make when it later reads this back.
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_talkback_probe_status = line;
        }
        return;
    }
    if (cmd == "talkback_session") {
        // F6 review-round fix: the session-side counterpart to
        // talkback_probe above -- engine/src/engine-talkback.cpp's
        // report_session()/report_session_state() tag every session/audio-
        // path line "cmd":"talkback_session" instead of "talkback_probe"
        // precisely so it stops overwriting the probe's status label and
        // logging as "talkback_probe: ..." when nothing to do with a probe
        // is happening. Log it under its own tag here.
        //
        // F2 review-round fix (CRITICAL): two distinct shapes share this
        // cmd -- report_session_state()'s confirmed-state line (has a
        // top-level "live" key, no "stage") and report_session()'s stage
        // trace lines (have "stage", never "live"). Tell them apart by the
        // presence of "live" rather than by ordering.
        if (obj.contains("live")) {
            const bool live = obj.value("live").toBool();
            const std::string reason = obj.value("reason").toString().toStdString();
            // TALKBACK DELIVERY LAW 1 (2026-08-29). The engine puts
            // "mic":"open"|"blocked" on THIS line -- the confirmed-state one --
            // precisely so the dock can tell "on air" from "on air but nobody
            // can hear you". Muted, SendAudioDataToChannel is ACCEPTED and
            // every member hears silence, so without this field the banner
            // says a clean ON AIR over a key that delivers nothing.
            //
            // ABSENT MEANS NOT BLOCKED. An engine older than Law 1 sends no
            // "mic" key, and a DLL-only install is this project's canonical
            // mistake -- reading a missing field as blocked would put every
            // such rig into a permanent false alarm. Same tolerance rule as
            // the nomination attempt id.
            //
            // REVIEW ROUND 1, M1 (Major): this branch read only live/reason.
            // The engine had emitted "mic" since the laws landed, three
            // comments asserted the plugin consumed it, a test pinned the
            // engine emitting it -- and nothing on this side had ever looked
            // at it, so Law 1's entire operator-facing half did not exist.
            const std::string mic = obj.value("mic").toString().toStdString();
            // The rule itself lives in src/talkback-key.h beside
            // talkback_session_state_closes_key(), so a host test can drive it
            // into the banner end to end -- see its comment for why that
            // matters here specifically.
            const bool mic_blocked = talkback_session_mic_blocked(mic);
            blog(LOG_INFO,
                 "[obs-zoom-plugin] talkback_session: live=%s reason=%s mic=%s",
                 live ? "true" : "false", reason.c_str(),
                 mic.empty() ? "(unreported)" : mic.c_str());
            std::lock_guard<std::mutex> lk(m_mtx);
            m_talkback_session_status.live        = live;
            m_talkback_session_status.reason      = reason;
            m_talkback_session_status.mic_blocked = mic_blocked;
            return;
        }
        blog(LOG_INFO, "[obs-zoom-plugin] talkback_session: %s", line.c_str());
        // Milestone 7 (the dock): two of these stage lines carry operator-
        // facing detail the confirmed-state line above does not have, and the
        // dock has nowhere else to get it. Everything else on this shape stays
        // log-only, exactly as before.
        //
        // "session_live" carries the keyed target's membership
        // (members_present/members_total, engine-talkback.cpp's
        // session_start()); the refusal line carries the engine's own recovery
        // hint ("recover":"re-nominate" for provisioning_incomplete). Both are
        // emitted BEFORE the report_session_state() line that sets `live`/
        // `reason` on the same path, so by the time the dock sees a reason the
        // hint that goes with it is already stored. Ordering is not relied on
        // for correctness though: talkback_start() clears this whole struct at
        // the start of every press, so the worst a re-ordering could do is
        // show a count one line late.
        const QString stage = obj.value("stage").toString();
        if (stage == QLatin1String("session_live")) {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_talkback_session_status.members_known = true;
            m_talkback_session_status.members_present =
                static_cast<uint32_t>(obj.value("members_present").toInt(0));
            m_talkback_session_status.members_total =
                static_cast<uint32_t>(obj.value("members_total").toInt(0));
        } else if (stage == QLatin1String("session_start") &&
                   obj.contains("recover")) {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_talkback_session_status.recover =
                obj.value("recover").toString().toStdString();
        }
        return;
    }
    if (cmd == "talkback_nominate") {
        // Task 5: mirrors talkback_probe's handling exactly -- log every
        // stage line verbatim (see the comment on the talkback_probe branch
        // above). Fix round 2 (N5): the stage-to-transition MAPPING (which
        // pure talkback-nomination.h function each report shape calls) is
        // factored into talkback_nomination_apply_report()
        // (src/talkback-nomination-dispatch.h) precisely so that mapping --
        // where both F1 and N1 actually lived -- can be driven by a host
        // test without the rest of this class. Do not inline stage-handling
        // logic back here; extend the dispatcher and its test instead.
        blog(LOG_INFO, "[obs-zoom-plugin] talkback_nominate: %s", line.c_str());
        std::lock_guard<std::mutex> lk(m_mtx);
        const QString nominate_stage = obj.value("stage").toString();
        talkback_nomination_apply_report(m_talkback_nomination_status,
            m_talkback_nomination_pending, nominate_stage, obj);
        // Milestone 7: the per-person presence view, from the SAME stage
        // lines and under the same lock. A separate call rather than a third
        // out-parameter above, because these stages carry no "attempt" id --
        // see talkback_channel_presence_apply_report()'s header comment.
        talkback_channel_presence_apply_report(m_talkback_channel_presence,
                                               nominate_stage, obj);
        return;
    }
    if (cmd == "awaiting_admission") {
        // Sent on every meeting-status change, so this is a plain assignment
        // and never needs an edge to clear it. See is_awaiting_admission().
        m_awaiting_admission.store(obj.value("active").toBool(),
                                   std::memory_order_release);
        return;
    }
    if (cmd == "joined") {
        m_awaiting_admission.store(false, std::memory_order_release);
        m_state.store(MeetingState::InMeeting, std::memory_order_release);
        // A successful join supersedes connection errors; without
        // this the dock keeps showing "Connection failed" from a previous
        // attempt over a perfectly healthy meeting. Breakout return also
        // emits joined: preserve source assignments and unresolved media
        // failures while the engine restores subscriptions itself.
        clear_last_error();
        ZoomReconnectManager::instance().on_join_success();
        return;
    }
    if (cmd == "left") {
        m_media_active.store(false, std::memory_order_release);
        // Belt and braces with the engine's own report: if the engine dies
        // while we are in a waiting room, no further status change is coming
        // and a stale true here would hold the watchdog off for the next join.
        m_awaiting_admission.store(false, std::memory_order_release);
        bool keep_failed = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roster.clear();
            m_active_speaker_id = 0;
            SpeakerDirector::instance().reset();
            // Task 5 fix round 1 (F2): the engine's own Leave path calls
            // nomination_reset() (engine/src/main.cpp) and destroys every
            // provisioned channel. This is the plugin-side world-reset that
            // already exists for exactly this moment -- join it here rather
            // than inventing a new hook. Without this, talkback_status kept
            // advertising the last meeting's plan after a Leave/rejoin, and
            // key_on()'s pre-check kept passing for targets that would now
            // refuse with "no_nomination".
            talkback_nomination_reset(m_talkback_nomination_status);
            m_talkback_nomination_pending = TalkbackNominationPending{};
            talkback_presence_reset(m_talkback_channel_presence);
            // A leave/rejoin starts a fresh handshake; a notice from the
            // previous meeting must not survive into it. No separate notify
            // call needed -- like the resets above, this is picked up by the
            // dock's own poll (pending_privilege_notice()) on its next tick,
            // same as this "left" handler has never notified roster callbacks
            // either (see this function's own doc comment).
            m_privilege_notice.clear();
            m_raw_media_error.clear();
            m_media_failures.reset();
            keep_failed = !m_last_error.empty() &&
                !m_user_leaving.load(std::memory_order_acquire);
        }
        m_state.store(keep_failed ? MeetingState::Failed : MeetingState::Idle,
                      std::memory_order_release);
        return;
    }
    if (cmd == "error" || cmd == "auth_fail") {
        // SDKERR_OTHER_SDK_INSTANCE_RUNNING out of InitSDK is a transient
        // collision, not an authentication failure: an orphaned ZoomObsEngine
        // from a previous OBS session still holds the Zoom SDK and exits on its
        // own moments later. That is what made the first "request engine" of a
        // session fail while the second worked. Wait it out (bounded) and
        // replay the init command instead of failing, and if the wait runs out
        // tell the operator what is actually wrong. Every other code and stage
        // falls straight through to the unchanged path below.
        if (cmd == "auth_fail") {
            const SdkInitRetryDecision retry_decision = decide_sdk_init_retry(
                obj.value("stage").toString().toStdString(),
                obj.value("code").toInt(0), m_init_retry_attempts);
            if (retry_decision.retry) {
                ++m_init_retry_attempts;
                m_init_retry_waited_ms += retry_decision.delay_ms;
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Zoom SDK init lost the race with another SDK "
                     "instance (SDKERR_OTHER_SDK_INSTANCE_RUNNING, code %d) — likely an "
                     "orphaned ZoomObsEngine. Retrying init in %llu ms (attempt %d/%d): %s",
                     kSdkErrOtherSdkInstanceRunning,
                     static_cast<unsigned long long>(retry_decision.delay_ms),
                     m_init_retry_attempts, kSdkInitRetryMaxAttempts, line.c_str());
                m_init_retry_due_ms.store(os_gettime_ns() / 1000000ULL +
                                              retry_decision.delay_ms,
                                          std::memory_order_release);
                return;
            }
            if (retry_decision.exhausted) {
                blog(LOG_ERROR,
                     "[obs-zoom-plugin] Zoom SDK init still blocked by another SDK "
                     "instance after %d attempts over %llus — stopping the engine so "
                     "the next request can relaunch it: %s",
                     kSdkInitRetryMaxAttempts,
                     static_cast<unsigned long long>(m_init_retry_waited_ms / 1000),
                     line.c_str());
                m_init_retry_due_ms.store(0, std::memory_order_release);
                // Hand the teardown and the operator-facing failure to the
                // monitor thread. This is the reader thread: stopping the
                // engine from here would self-join it.
                m_init_teardown_pending.store(true, std::memory_order_release);
                return;
            }
        }
        blog(LOG_ERROR, "[obs-zoom-plugin] Zoom engine event: %s", line.c_str());
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_debug_events.push_back({os_gettime_ns() / 1000000ULL,
                obj.value("msg").toString().toStdString(),
                obj.value("source_uuid").toString().toStdString(),
                static_cast<uint32_t>(obj.value("participant_id").toInt()), line});
            while (m_debug_events.size() > 300) m_debug_events.pop_front();
        }
        const QString emsg = obj.value("msg").toString();
        // Media-path errors: the meeting itself is still healthy, so surface
        // them loudly to the operator but do NOT tear the session down or
        // trigger the reconnect flow.
        if (zoom_persistent_source_media_failure(emsg.toStdString())) {
            const std::string uuid =
                obj.value("source_uuid").toString().toStdString();
            std::string error_message;
            if (emsg == "shm_create_failed") {
                error_message =
                    "Zoom engine could not allocate shared memory for source " +
                    (uuid.empty() ? std::string("(unknown)") : uuid) +
                    " — its frames are being dropped";
            } else if (emsg == "shm_name_collision") {
                // engine-audio.cpp::ensure_shm() — the engine's "create" opened
                // a section another process still holds, i.e. a ghost
                // ZoomObsEngine survived the pre-launch sweep. The audio it
                // writes into that shared ring suppresses this engine's edge
                // notifications (2026-08-17 root cause). The meeting itself is
                // healthy: tell the operator, do not tear anything down.
                error_message =
                    "A stale ZoomObsEngine process is sharing audio memory "
                    "with source " +
                    (uuid.empty() ? std::string("(unknown)") : uuid) +
                    " — audio will stutter until it is killed. Restart the "
                    "engine (or end stray ZoomObsEngine.exe processes).";
            } else {
                const int limit = obj.value("limit").toInt(0);
                error_message =
                    "Zoom engine rejected subscription for source " +
                    (uuid.empty() ? std::string("(unknown)") : uuid) +
                    ": too many active sources" +
                    (limit > 0 ? " (limit " + std::to_string(limit) + ")"
                               : std::string());
            }
            std::vector<NoticeCallback> callbacks;
            std::string notice;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_media_failures.persistent_fail(uuid, error_message,
                        m_sources.find(uuid) != m_sources.end()) && m_privilege_notice.empty()) {
                    notice = m_media_failures.status(os_gettime_ns() / 1000000ULL);
                    for (const auto &entry : m_notice_callbacks)
                        if (entry.second) callbacks.push_back(entry.second);
                }
            }
            for (const auto &cb : callbacks) cb(notice);
            return;
        }
        // Another media-path error: sources keep their participant binding
        // across meetings, but Zoom mints user ids per join, so after a
        // rejoin a saved id often refers to nobody until the operator
        // reassigns the source or that participant comes back. Subscribing
        // to an absent participant is therefore a waiting state, not an
        // error — stay quiet and let the recovery loop keep retrying. If the
        // participant IS in the roster it joins the current recovery episode,
        // but either way the meeting itself is healthy: never route this
        // into the join-failure / reconnect machinery below (doing so
        // flipped the session to "Connection failed" mid-meeting).
        if (zoom_source_video_failure(emsg.toStdString())) {
            const uint32_t participant_id =
                static_cast<uint32_t>(obj.value("participant_id").toInt());
            const std::string uuid =
                obj.value("source_uuid").toString().toStdString();
            bool first = false;
            std::vector<NoticeCallback> callbacks;
            std::string notice;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                const bool known = std::any_of(m_roster.begin(), m_roster.end(),
                    [&](const ParticipantInfo &p) { return p.user_id == participant_id; });
                first = m_media_failures.fail(uuid, participant_id,
                    emsg.toStdString() + " (code " + std::to_string(obj.value("code").toInt()) + ")",
                    os_gettime_ns() / 1000000ULL, known);
                if (first && m_privilege_notice.empty()) {
                    notice = m_media_failures.status(os_gettime_ns() / 1000000ULL);
                    for (const auto &entry : m_notice_callbacks)
                        if (entry.second) callbacks.push_back(entry.second);
                }
            }
            for (const auto &cb : callbacks) cb(notice);
            return;
        }
        dispatch_zoom_engine_failure(cmd.toStdString(), emsg.toStdString(),
            obj.value("privilege_requested").toBool(),
            [&](bool pending) {
                // Media failure never votes against meeting/reconnect state.
                // Store terminal diagnostics separately: the left handler reads
                // m_last_error as proof that the meeting failed.
                m_media_active.store(false, std::memory_order_release);
                const auto detail = obj.value("detail").toString().toStdString();
                const auto reason = obj.value("reason").toString();
                const auto message = pending ? zoom_privilege_notice_text(detail) :
                    (reason == "privilege_denied"
                        ? std::string("Recording permission denied. Ask the host to allow recording; media will start automatically when granted.")
                        : reason == "privilege_request_timeout"
                        ? std::string("Recording permission request timed out. Ask the host to allow recording, then click Retry Media.")
                        : (detail.empty() ? zoom_error_message(obj) : detail) + " Click Retry Media.");
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_raw_media_error = pending ? std::string() : message;
                }
                set_privilege_notice_and_notify(message);
            }, [&] {
                const QString reason = obj.value("reason").toString();
                const int code = obj.value("code").toInt(0);
                set_error_and_notify(zoom_error_message(obj));
                // Permanent failures: auth, license, host-ended.
                if (cmd == "auth_fail" || reason == "auth_fail") {
                    m_state.store(MeetingState::Failed, std::memory_order_release);
                    ZoomReconnectManager::instance().on_join_failed(true);
                } else if (obj.value("msg").toString() == "meeting_failed" &&
                           is_permanent_meeting_failure(code)) {
                    m_state.store(MeetingState::Failed, std::memory_order_release);
                    blog(LOG_ERROR,
                         "[obs-zoom-plugin] Permanent Zoom meeting failure %d (%s) - not retrying",
                         code, reason.toUtf8().constData());
                    ZoomReconnectManager::instance().on_join_failed(true);
                } else if (reason == "license") {
                    m_state.store(MeetingState::Failed, std::memory_order_release);
                    ZoomReconnectManager::instance().trigger(RecoveryReason::LicenseError);
                } else if (reason == "host_ended") {
                    ZoomReconnectManager::instance().trigger(RecoveryReason::HostEndedMeeting);
                } else {
                    // Retriable failure — let reconnect manager decide.
                    if (!m_user_leaving.load(std::memory_order_acquire)) {
                        ZoomReconnectManager::instance().on_join_failed(false);
                    } else {
                        m_state.store(MeetingState::Failed, std::memory_order_release);
                    }
                }
            });
        return;
    }

    if (cmd == "participants") {
        // Roster callbacks are dispatched by the helper with m_mtx released;
        // see update_roster_state_and_notify() for why that is mandatory.
        update_roster_state_and_notify([this, &obj] {
            m_active_speaker_id = static_cast<uint32_t>(
                obj.value("active_speaker_id").toInt());
            m_roster.clear();
            const QJsonArray participants = obj.value("participants").toArray();
            m_roster.reserve(static_cast<size_t>(participants.size()));
            for (const QJsonValue &value : participants) {
                const QJsonObject po = value.toObject();
                ParticipantInfo p;
                p.user_id = static_cast<uint32_t>(po.value("id").toInt());
                p.display_name = po.value("name").toString().toStdString();
                p.has_video = po.value("has_video").toBool();
                p.is_talking = po.value("is_talking").toBool();
                p.is_muted = po.value("is_muted").toBool();
                p.is_host = po.value("is_host").toBool();
                p.is_co_host = po.value("is_co_host").toBool();
                p.raised_hand = po.value("raised_hand").toBool();
                p.spotlight_index = static_cast<uint32_t>(po.value("spotlight").toInt());
                p.is_sharing_screen = po.value("is_sharing_screen").toBool();
                m_roster.push_back(std::move(p));
            }
            m_media_failures.prune([&](uint32_t id) {
                return std::any_of(m_roster.begin(), m_roster.end(),
                    [&](const ParticipantInfo &p) { return p.user_id == id; });
            });
            SpeakerDirector::instance().update_roster(
                m_roster, m_active_speaker_id, os_gettime_ns() / 1000000ULL);
        });
        return;
    }
    if (cmd == "active_speaker") {
        update_roster_state_and_notify([this, &obj] {
            m_active_speaker_id = static_cast<uint32_t>(
                obj.value("participant_id").toInt());
            for (auto &p : m_roster)
                p.is_talking = p.user_id == m_active_speaker_id;
            SpeakerDirector::instance().update_roster(
                m_roster, m_active_speaker_id, os_gettime_ns() / 1000000ULL);
        });
        return;
    }

    if (cmd != "frame" && cmd != "audio") return;
    const std::string uuid = obj.value("source_uuid").toString().toStdString();
    // No callback lookup here: the reader thread's whole job for a media
    // event is to enqueue the prompt and get back to the pipe. Resolution to
    // callbacks happens on the lane thread (dispatch_media_event), which also
    // absorbs the enqueue-vs-unregister race as a lookup miss. shm_gen guards
    // against reading a stale/orphaned SHM region after the engine recreated
    // it (0 = not sent, old engine); it rides in the event so the handler can
    // remap before it drains.
    if (cmd == "frame") {
        static std::mutex frame_log_mtx;
        uint64_t frame_count = 0;
        {
            std::lock_guard<std::mutex> lk(frame_log_mtx);
            static std::unordered_map<std::string, uint64_t> frame_counts;
            frame_count = ++frame_counts[uuid];
        }
        if (cv_zoom_verbose_logging() &&
            (frame_count == 1 || frame_count % 120 == 0)) {
            blog(LOG_INFO,
                 "[obs-zoom-plugin] Queueing Zoom video frame: source_uuid=%s count=%llu w=%d h=%d coalesced=%llu",
                 uuid.c_str(), static_cast<unsigned long long>(frame_count),
                 obj.value("w").toInt(), obj.value("h").toInt(),
                 static_cast<unsigned long long>(
                     m_video_lane.queue.coalesced()));
        }
        m_video_lane.push(uuid, MediaEvent{
            static_cast<uint32_t>(obj.value("w").toInt()),
            static_cast<uint32_t>(obj.value("h").toInt()),
            static_cast<uint32_t>(obj.value("participant_id").toInt()),
            static_cast<uint32_t>(obj.value("shm_gen").toInt())});
    } else {
        m_audio_lane.push(uuid, MediaEvent{
            static_cast<uint32_t>(obj.value("byte_len").toInt()),
            static_cast<uint32_t>(obj.value("participant_id").toInt()),
            static_cast<uint32_t>(obj.value("shm_gen").toInt()), 0});
    }
}

void ZoomEngineClient::send_join_locked()
{
    if (!m_join_pending || m_pending_meeting_id.empty()) return;

    const char *kind_str = (m_pending_kind == MeetingKind::Webinar) ? "webinar" : "meeting";
    blog(LOG_INFO, "[obs-zoom-plugin] Sending Zoom join to engine: meeting_id=%s kind=%s",
         m_pending_meeting_id.c_str(), kind_str);
    std::string json = R"({"cmd":"join","meeting_id":")" + json_escape(m_pending_meeting_id) +
        R"(","passcode":")" + json_escape(m_pending_passcode) +
        R"(","display_name":")" + json_escape(m_pending_display_name) +
        R"(","kind":")" + kind_str + "\"";
    if (!m_pending_tokens.on_behalf_token.empty()) {
        json += R"(,"on_behalf_token":")" +
            json_escape(m_pending_tokens.on_behalf_token) + "\"";
    }
    if (!m_pending_tokens.user_zak.empty()) {
        json += R"(,"user_zak":")" + json_escape(m_pending_tokens.user_zak) + "\"";
    }
    if (!m_pending_tokens.app_privilege_token.empty()) {
        json += R"(,"app_privilege_token":")" +
            json_escape(m_pending_tokens.app_privilege_token) + "\"";
    }
    json += "}";
    if (m_p2e == kIpcInvalidFd || !ipc_write_line(m_p2e, json)) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Failed to send join to engine; link is broken");
        m_last_error = "Lost connection to Zoom engine";
        if (m_p2e != kIpcInvalidFd) {
#if defined(WIN32)
            CloseHandle(m_p2e); m_p2e = kIpcInvalidFd;
#else
            close(m_p2e); m_p2e = kIpcInvalidFd;
#endif
        }
        return;
    }
    m_join_pending = false;
}

uint32_t ZoomEngineClient::active_speaker_id() const
{
    return SpeakerDirector::instance().directed_speaker_id();
}

uint32_t ZoomEngineClient::raw_active_speaker_id() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_active_speaker_id;
}

std::string ZoomEngineClient::last_error() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    // Control status and diagnostics still expose terminal media failure,
    // while internal meeting classification uses only m_last_error.
    if (!m_last_error.empty()) return m_last_error;
    if (!m_raw_media_error.empty()) return m_raw_media_error;
    const auto now = os_gettime_ns() / 1000000ULL;
    return m_media_failures.terminal(now) ? m_media_failures.status(now) : std::string();
}

std::string ZoomEngineClient::pending_privilege_notice() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_privilege_notice.empty()) return m_privilege_notice;
    return m_media_failures.status(os_gettime_ns() / 1000000ULL);
}

void ZoomEngineClient::clear_last_error()
{
    std::vector<ErrorCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_last_error.clear();
        for (const auto &entry : m_error_callbacks)
            if (entry.second) callbacks.push_back(entry.second);
    }
    for (const auto &cb : callbacks) cb(std::string());
}

std::vector<ParticipantInfo> ZoomEngineClient::roster() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_roster;
}

std::vector<ZoomEngineClient::DebugEvent>
ZoomEngineClient::recent_debug_events() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return {m_debug_events.begin(), m_debug_events.end()};
}

void ZoomEngineClient::add_roster_callback(void *key, RosterCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb)
        m_roster_callbacks[key] = std::move(cb);
    else
        m_roster_callbacks.erase(key);
}

void ZoomEngineClient::remove_roster_callback(void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_roster_callbacks.erase(key);
}

void ZoomEngineClient::add_error_callback(void *key, ErrorCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb)
        m_error_callbacks[key] = std::move(cb);
    else
        m_error_callbacks.erase(key);
}

void ZoomEngineClient::remove_error_callback(void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_error_callbacks.erase(key);
}

void ZoomEngineClient::add_notice_callback(void *key, NoticeCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb)
        m_notice_callbacks[key] = std::move(cb);
    else
        m_notice_callbacks.erase(key);
}

void ZoomEngineClient::remove_notice_callback(void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_notice_callbacks.erase(key);
}

bool ZoomEngineClient::write_json(const std::string &json)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_p2e == kIpcInvalidFd) return false;
    if (ipc_write_line(m_p2e, json))
        return true;
    // The command did not reach the engine — the pipe is broken. Surface it and
    // close our end so the reader loop unblocks and the monitor/reconnect path
    // can recover instead of silently desyncing.
    blog(LOG_ERROR,
         "[obs-zoom-plugin] IPC write to engine failed; tearing down link for recovery");
    m_last_error = "Lost connection to Zoom engine";
#if defined(WIN32)
    CloseHandle(m_p2e); m_p2e = kIpcInvalidFd;
#else
    close(m_p2e); m_p2e = kIpcInvalidFd;
#endif
    return false;
}
