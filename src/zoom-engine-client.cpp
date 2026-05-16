#include "zoom-engine-client.h"
#include "zoom-reconnect.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <obs-module.h>
#include <util/platform.h>
#include <chrono>
#include <thread>
#include <unordered_map>

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

static std::string zoom_error_message(const QJsonObject &obj)
{
    const QString msg = obj.value("msg").toString();
    const QString reason = obj.value("reason").toString();
    const int code = obj.value("code").toInt(0);

    if (msg == "meeting_failed") {
        if (code == 63) {
            return "Zoom rejected the join: external meetings require a published "
                   "Meeting SDK app or Zoom approval. (63 "
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
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
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

ZoomEngineClient::~ZoomEngineClient()
{
    stop();
}

bool ZoomEngineClient::start(const std::string &jwt_token)
{
    if (m_running.load(std::memory_order_acquire)) return true;
    if (jwt_token.empty()) {
        const std::string message = "Cannot start Zoom engine: JWT token is empty";
        set_last_error(message);
        blog(LOG_ERROR, "[obs-zoom-plugin] %s", message.c_str());
        return false;
    }
    m_last_jwt = jwt_token;
    m_user_leaving.store(false, std::memory_order_release);
    m_authenticated.store(false, std::memory_order_release);
    m_media_active.store(false, std::memory_order_release);

    // Join threads from any previous session (e.g. after a crash).
    if (m_reader.joinable())  m_reader.join();
    if (m_monitor.joinable()) m_monitor.join();

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

    m_running.store(true, std::memory_order_release);
    m_reader  = std::thread([this]() { reader_loop(); });
    m_monitor = std::thread([this]() { monitor_loop(); });
    write_json(R"({"cmd":"init","jwt":")" + json_escape(jwt_token) + "\"}");
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
    m_authenticated.store(false, std::memory_order_release);
    m_media_active.store(false, std::memory_order_release);
    m_state.store(MeetingState::Idle, std::memory_order_release);
}

void ZoomEngineClient::monitor_loop()
{
    // Wait for the engine process to exit.
    int exit_code = 0;
#if defined(WIN32)
    if (m_process) {
        WaitForSingleObject(static_cast<HANDLE>(m_process), INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(static_cast<HANDLE>(m_process), &code);
        exit_code = static_cast<int>(code);
        CloseHandle(static_cast<HANDLE>(m_process));
        m_process = nullptr;
    }
#else
    if (m_pid > 0) {
        int status = 0;
        waitpid(m_pid, &status, 0);
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        m_pid = -1;
    }
#endif

    // If m_running is still true the engine exited without being asked to.
    if (m_running.exchange(false, std::memory_order_acq_rel)) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] ZoomObsEngine exited unexpectedly (code %d)",
             exit_code);
        disconnect_ipc(); // unblocks reader_loop so it exits cleanly

        // If the user is in the middle of leaving / stopping, don't try to recover —
        // we lost a race against stop() but the user's intent is clear.
        if (m_user_leaving.load(std::memory_order_acquire)) {
            m_state.store(MeetingState::Idle, std::memory_order_release);
            return;
        }

        RecoveryReason reason = RecoveryReason::EngineCrash;
        if (exit_code == 2) reason = RecoveryReason::AuthFailure;
        else if (exit_code == 3) reason = RecoveryReason::SdkError;
        else if (exit_code == 4) reason = RecoveryReason::LicenseError;

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
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","mode":"spotlight","slot":)" + std::to_string(slot) + "}");
}

void ZoomEngineClient::subscribe_screenshare(const std::string &source_uuid)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","mode":"screenshare"})");
}

void ZoomEngineClient::leave()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    m_user_leaving.store(true, std::memory_order_release);
    ZoomReconnectManager::instance().cancel(); // suppress any in-progress recovery
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

void ZoomEngineClient::subscribe(const std::string &source_uuid,
                                 uint32_t participant_id,
                                 bool isolate_audio,
                                 VideoResolution video_resolution)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","participant_id":)" + std::to_string(participant_id) +
        R"(,"resolution":)" + std::to_string(static_cast<int>(video_resolution)) +
        R"(,"isolate_audio":)" + std::string(isolate_audio ? "true" : "false") +
        "}");
}

void ZoomEngineClient::unsubscribe(const std::string &source_uuid)
{
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
}

bool ZoomEngineClient::launch_engine()
{
#if defined(WIN32)
    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
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
    const pid_t pid = fork();
    if (pid < 0) {
        set_last_error("Failed to fork ZoomObsEngine process.");
        return false;
    }
    if (pid == 0) {
        const std::string path = engine_executable_path();
        execlp(path.c_str(), path.c_str(), nullptr);
        _exit(127);
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
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line));
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString cmd = obj.value("cmd").toString();

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
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom engine debug: %s", line.c_str());
        const QString stage = obj.value("stage").toString();
        if (stage == "raw_media_ready")
            m_media_active.store(true, std::memory_order_release);
        else if (stage == "raw_media_stopped")
            m_media_active.store(false, std::memory_order_release);
        return;
    }
    if (cmd == "joined") {
        m_state.store(MeetingState::InMeeting, std::memory_order_release);
        ZoomReconnectManager::instance().on_join_success();
        return;
    }
    if (cmd == "left") {
        m_media_active.store(false, std::memory_order_release);
        bool keep_failed = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roster.clear();
            m_active_speaker_id = 0;
            keep_failed = !m_last_error.empty() &&
                !m_user_leaving.load(std::memory_order_acquire);
        }
        m_state.store(keep_failed ? MeetingState::Failed : MeetingState::Idle,
                      std::memory_order_release);
        return;
    }
    if (cmd == "error" || cmd == "auth_fail") {
        blog(LOG_ERROR, "[obs-zoom-plugin] Zoom engine event: %s", line.c_str());
        const QString reason = obj.value("reason").toString();
        const int code = obj.value("code").toInt(0);
        std::vector<ErrorCallback> error_callbacks;
        const std::string error_message = zoom_error_message(obj);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_last_error = error_message;
            for (const auto &entry : m_error_callbacks)
                if (entry.second) error_callbacks.push_back(entry.second);
        }
        for (auto &cb : error_callbacks) cb(error_message);
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
        return;
    }

    if (cmd == "participants") {
        std::vector<RosterCallback> callbacks;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
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
            for (const auto &entry : m_roster_callbacks)
                if (entry.second) callbacks.push_back(entry.second);
        }
        for (auto &cb : callbacks) cb();
        return;
    }
    if (cmd == "active_speaker") {
        std::vector<RosterCallback> callbacks;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_active_speaker_id = static_cast<uint32_t>(
                obj.value("participant_id").toInt());
            for (auto &p : m_roster)
                p.is_talking = p.user_id == m_active_speaker_id;
            for (const auto &entry : m_roster_callbacks)
                if (entry.second) callbacks.push_back(entry.second);
        }
        for (auto &cb : callbacks) cb();
        return;
    }

    if (cmd != "frame" && cmd != "audio") return;
    const std::string uuid = obj.value("source_uuid").toString().toStdString();
    SourceCallbacks callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_sources.find(uuid);
        if (it == m_sources.end()) return;
        callbacks = it->second;
    }
    if (cmd == "frame" && callbacks.on_frame) {
        static std::mutex frame_log_mtx;
        static std::unordered_map<std::string, uint64_t> frame_counts;
        uint64_t frame_count = 0;
        {
            std::lock_guard<std::mutex> lk(frame_log_mtx);
            frame_count = ++frame_counts[uuid];
        }
        if (frame_count == 1 || frame_count % 120 == 0) {
            blog(LOG_INFO,
                 "[obs-zoom-plugin] Dispatching Zoom video frame: source_uuid=%s count=%llu w=%d h=%d",
                 uuid.c_str(), static_cast<unsigned long long>(frame_count),
                 obj.value("w").toInt(), obj.value("h").toInt());
        }
        callbacks.on_frame(static_cast<uint32_t>(obj.value("w").toInt()),
                           static_cast<uint32_t>(obj.value("h").toInt()));
    }
    if (cmd == "audio" && callbacks.on_audio) {
        callbacks.on_audio(static_cast<uint32_t>(obj.value("byte_len").toInt()));
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
    ipc_write_line(m_p2e, json);
    m_join_pending = false;
}

uint32_t ZoomEngineClient::active_speaker_id() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_active_speaker_id;
}

std::string ZoomEngineClient::last_error() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_last_error;
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
    for (auto &cb : callbacks) cb(std::string());
}

std::vector<ParticipantInfo> ZoomEngineClient::roster() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_roster;
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

void ZoomEngineClient::write_json(const std::string &json)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_p2e == kIpcInvalidFd) return;
    ipc_write_line(m_p2e, json);
}
