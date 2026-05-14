#include "zoom-engine-client.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <obs-module.h>
#include <util/platform.h>
#include <chrono>
#include <thread>

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
            std::string candidate = path.substr(0, slash + 1) + "ZoomObsEngine.exe";
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
        blog(LOG_ERROR, "[obs-zoom-plugin] Cannot start Zoom engine: JWT token is empty");
        return false;
    }

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
    m_reader = std::thread([this]() { reader_loop(); });
    write_json(R"({"cmd":"init","jwt":")" + json_escape(jwt_token) + "\"}");
    return true;
}

void ZoomEngineClient::stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    write_json(R"({"cmd":"quit"})");
    disconnect_ipc();
    if (m_reader.joinable()) m_reader.join();
    m_state.store(MeetingState::Idle, std::memory_order_release);

#if defined(WIN32)
    if (m_process) {
        CloseHandle(static_cast<HANDLE>(m_process));
        m_process = nullptr;
    }
#else
    if (m_pid > 0) {
        waitpid(m_pid, nullptr, 0);
        m_pid = -1;
    }
#endif
}

bool ZoomEngineClient::join(const std::string &meeting_id,
                            const std::string &passcode,
                            const std::string &display_name)
{
    if (!m_running.load(std::memory_order_acquire)) return false;
    if (meeting_id.empty()) return false;
    m_state.store(MeetingState::Joining, std::memory_order_release);
    write_json(R"({"cmd":"join","meeting_id":")" + json_escape(meeting_id) +
        R"(","passcode":")" + json_escape(passcode) +
        R"(","display_name":")" + json_escape(display_name) + "\"}");
    return true;
}

void ZoomEngineClient::leave()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    m_state.store(MeetingState::Leaving, std::memory_order_release);
    write_json(R"({"cmd":"leave"})");
}

void ZoomEngineClient::subscribe(const std::string &source_uuid, uint32_t participant_id)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","participant_id":)" + std::to_string(participant_id) + "}");
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
    std::string command = "\"" + engine_executable_path() + "\"";

    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Failed to launch ZoomObsEngine: %lu",
             GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) return false;
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
    return false;
#endif
}

void ZoomEngineClient::disconnect_ipc()
{
#if defined(WIN32)
    if (m_p2e != kIpcInvalidFd) CloseHandle(m_p2e);
    if (m_e2p != kIpcInvalidFd) CloseHandle(m_e2p);
#else
    if (m_p2e != kIpcInvalidFd) close(m_p2e);
    if (m_e2p != kIpcInvalidFd) close(m_e2p);
#endif
    m_p2e = kIpcInvalidFd;
    m_e2p = kIpcInvalidFd;
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

    if (cmd == "auth_ok") {
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom engine authenticated");
        return;
    }
    if (cmd == "joined") {
        m_state.store(MeetingState::InMeeting, std::memory_order_release);
        return;
    }
    if (cmd == "left") {
        m_state.store(MeetingState::Idle, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roster.clear();
            m_active_speaker_id = 0;
        }
        return;
    }
    if (cmd == "error" || cmd == "auth_fail") {
        m_state.store(MeetingState::Failed, std::memory_order_release);
        blog(LOG_ERROR, "[obs-zoom-plugin] Zoom engine event: %s", line.c_str());
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
        callbacks.on_frame(static_cast<uint32_t>(obj.value("w").toInt()),
                           static_cast<uint32_t>(obj.value("h").toInt()));
    }
    if (cmd == "audio" && callbacks.on_audio) {
        callbacks.on_audio(static_cast<uint32_t>(obj.value("byte_len").toInt()));
    }
}

uint32_t ZoomEngineClient::active_speaker_id() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_active_speaker_id;
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

void ZoomEngineClient::write_json(const std::string &json)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_p2e == kIpcInvalidFd) return;
    ipc_write_line(m_p2e, json);
}
