#include "iso-ffmpeg-pipe.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#endif

#ifdef _WIN32

// Windows command-line quoting per the CommandLineToArgvW rules: quote when
// needed, double backslashes only when they precede a quote (or the closing
// quote), escape embedded quotes.
static void append_quoted_arg(std::wstring &cmd, const std::wstring &arg)
{
    if (!cmd.empty())
        cmd += L' ';
    if (!arg.empty() &&
        arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
            backslashes = 0;
            continue;
        }
        cmd.append(backslashes, L'\\');
        backslashes = 0;
        cmd += c;
    }
    cmd.append(backslashes * 2, L'\\');
    cmd += L'"';
}

static std::wstring widen_utf8(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), wide.data(), n);
    return wide;
}

bool IsoFfmpegPipe::start(const std::string &program,
                          const std::vector<std::string> &args,
                          const std::string &log_path,
                          size_t max_queued_bytes,
                          std::string *error)
{
    if (m_started.load(std::memory_order_acquire)) {
        if (error) *error = "already started";
        return false;
    }
    m_log_path = log_path;
    m_max_queued_bytes = max_queued_bytes;

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &inherit, 0)) {
        if (error) *error = "CreatePipe failed, error " +
                            std::to_string(GetLastError());
        return false;
    }
    // Only the READ end goes to the child; keeping the write end
    // non-inheritable is what makes the child see EOF when we close ours.
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    // stdout+stderr -> log file. The child owns its handle; a failure to
    // open the log must not stop the recording, so fall back to NUL.
    HANDLE log = CreateFileW(widen_utf8(log_path).c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        log = CreateFileW(L"NUL", GENERIC_WRITE, 0, &inherit, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring cmd;
    append_quoted_arg(cmd, widen_utf8(program));
    for (const std::string &a : args)
        append_quoted_arg(cmd, widen_utf8(a));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = log;
    si.hStdError = log;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const DWORD create_error = GetLastError();

    // The child holds its own duplicates now (or was never created).
    CloseHandle(stdin_read);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);

    if (!ok) {
        CloseHandle(stdin_write);
        if (error) *error = "CreateProcess failed, error " +
                            std::to_string(create_error);
        return false;
    }
    CloseHandle(pi.hThread);

    m_process = pi.hProcess;
    m_stdin_write = stdin_write;
    m_pid = static_cast<long long>(pi.dwProcessId);
    m_started.store(true, std::memory_order_release);
    m_writer = std::thread([this] { writer_loop(); });
    return true;
}

bool IsoFfmpegPipe::running() const
{
    if (!m_started.load(std::memory_order_acquire) || !m_process)
        return false;
    return WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

bool IsoFfmpegPipe::wait_finished(int timeout_ms)
{
    if (!m_started.load(std::memory_order_acquire) || !m_process)
        return true;
    return WaitForSingleObject(m_process,
                               timeout_ms < 0 ? INFINITE
                                              : static_cast<DWORD>(timeout_ms))
           == WAIT_OBJECT_0;
}

void IsoFfmpegPipe::kill()
{
    if (!m_started.load(std::memory_order_acquire) || !m_process)
        return;
    m_killed.store(true, std::memory_order_release);
    TerminateProcess(m_process, 62097);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
        m_queue.clear();
        m_queued_bytes = 0;
    }
    m_cv.notify_all();
}

int IsoFfmpegPipe::exit_code() const
{
    const int cached = m_exit_code.load(std::memory_order_acquire);
    if (cached != -1)
        return cached;
    if (!m_process)
        return -1;
    DWORD code = 0;
    if (!GetExitCodeProcess(m_process, &code) || code == STILL_ACTIVE)
        return -1;
    m_exit_code.store(static_cast<int>(code), std::memory_order_release);
    return static_cast<int>(code);
}

bool IsoFfmpegPipe::crashed() const
{
    if (m_killed.load(std::memory_order_acquire))
        return true;
    const int code = exit_code();
    // NTSTATUS-style faults (0xC0000005 access violation and friends) come
    // back as large negative ints.
    return code != -1 && static_cast<uint32_t>(code) >= 0xC0000000u;
}

void IsoFfmpegPipe::close_stdin_handle()
{
    if (m_stdin_write) {
        CloseHandle(static_cast<HANDLE>(m_stdin_write));
        m_stdin_write = nullptr;
    }
}

static bool write_all(void *handle, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        DWORD wrote = 0;
        if (!WriteFile(static_cast<HANDLE>(handle), data + off,
                       static_cast<DWORD>(len - off), &wrote, nullptr))
            return false;
        off += wrote;
    }
    return true;
}

#else // POSIX

bool IsoFfmpegPipe::start(const std::string &program,
                          const std::vector<std::string> &args,
                          const std::string &log_path,
                          size_t max_queued_bytes,
                          std::string *error)
{
    if (m_started.load(std::memory_order_acquire)) {
        if (error) *error = "already started";
        return false;
    }
    m_log_path = log_path;
    m_max_queued_bytes = max_queued_bytes;

    int fds[2];
    if (pipe(fds) != 0) {
        if (error) *error = "pipe() failed";
        return false;
    }
    int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0)
        log_fd = open("/dev/null", O_WRONLY);

    const pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        if (log_fd >= 0) close(log_fd);
        if (error) *error = "fork() failed";
        return false;
    }
    if (child == 0) {
        dup2(fds[0], STDIN_FILENO);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        close(fds[0]);
        close(fds[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const std::string &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    close(fds[0]);
    if (log_fd >= 0)
        close(log_fd);
    // The writer must see EPIPE as an error return, not a process-fatal
    // signal, when the child dies mid-recording.
    signal(SIGPIPE, SIG_IGN);

    m_pid = child;
    m_stdin_write = reinterpret_cast<void *>(
        static_cast<intptr_t>(fds[1]) + 1); // +1 so fd 0 is distinguishable
    m_started.store(true, std::memory_order_release);
    m_writer = std::thread([this] { writer_loop(); });
    return true;
}

bool IsoFfmpegPipe::running() const
{
    if (!m_started.load(std::memory_order_acquire) || m_pid < 0)
        return false;
    if (m_exit_code.load(std::memory_order_acquire) != -1)
        return false;
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
    if (r == static_cast<pid_t>(m_pid)) {
        m_exit_code.store(WIFEXITED(status) ? WEXITSTATUS(status)
                                            : 128 + WTERMSIG(status),
                          std::memory_order_release);
        return false;
    }
    return r == 0;
}

bool IsoFfmpegPipe::wait_finished(int timeout_ms)
{
    const int step_ms = 20;
    int waited = 0;
    while (running()) {
        if (timeout_ms >= 0 && waited >= timeout_ms)
            return false;
        struct timespec ts { 0, step_ms * 1000000 };
        nanosleep(&ts, nullptr);
        waited += step_ms;
    }
    return true;
}

void IsoFfmpegPipe::kill()
{
    if (!m_started.load(std::memory_order_acquire) || m_pid < 0)
        return;
    m_killed.store(true, std::memory_order_release);
    ::kill(static_cast<pid_t>(m_pid), SIGKILL);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
        m_queue.clear();
        m_queued_bytes = 0;
    }
    m_cv.notify_all();
}

int IsoFfmpegPipe::exit_code() const
{
    running(); // reaps and caches on exit
    return m_exit_code.load(std::memory_order_acquire);
}

bool IsoFfmpegPipe::crashed() const
{
    if (m_killed.load(std::memory_order_acquire))
        return true;
    const int code = exit_code();
    return code > 128; // died on a signal
}

void IsoFfmpegPipe::close_stdin_handle()
{
    if (m_stdin_write) {
        close(static_cast<int>(
                  reinterpret_cast<intptr_t>(m_stdin_write)) - 1);
        m_stdin_write = nullptr;
    }
}

static bool write_all(void *handle, const uint8_t *data, size_t len)
{
    const int fd = static_cast<int>(
                       reinterpret_cast<intptr_t>(handle)) - 1;
    size_t off = 0;
    while (off < len) {
        const ssize_t wrote = ::write(fd, data + off, len - off);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += static_cast<size_t>(wrote);
    }
    return true;
}

#endif // _WIN32 / POSIX

// ── shared ───────────────────────────────────────────────────────────────────

IsoFfmpegPipe::~IsoFfmpegPipe()
{
    if (m_started.load(std::memory_order_acquire)) {
        if (running())
            kill();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
        }
        m_cv.notify_all();
    }
    if (m_writer.joinable())
        m_writer.join();
    close_stdin_handle();
#ifdef _WIN32
    if (m_process) {
        CloseHandle(static_cast<HANDLE>(m_process));
        m_process = nullptr;
    }
#endif
}

bool IsoFfmpegPipe::try_queue(std::vector<uint8_t> &&buf)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_broken || m_stop || m_eof_requested)
        return false;
    if (m_queued_bytes + buf.size() > m_max_queued_bytes)
        return false;
    m_queued_bytes += buf.size();
    m_queue.push_back(std::move(buf));
    m_cv.notify_one();
    return true;
}

size_t IsoFfmpegPipe::queued_bytes() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_queued_bytes;
}

void IsoFfmpegPipe::close_stdin()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_eof_requested = true;
    }
    m_cv.notify_all();
}

void IsoFfmpegPipe::writer_loop()
{
    for (;;) {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] {
                return !m_queue.empty() || m_eof_requested || m_stop;
            });
            if (m_stop)
                break;
            if (m_queue.empty()) {
                // EOF requested and the queue is drained.
                break;
            }
            buf = std::move(m_queue.front());
            m_queue.pop_front();
            m_queued_bytes -= buf.size();
        }
        // Blocking write, deliberately outside the lock: a stalled child
        // parks THIS thread only; callers keep getting instant
        // try_queue() == false once the byte bound is hit.
        if (!m_broken && m_stdin_write &&
            !write_all(m_stdin_write, buf.data(), buf.size())) {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_broken = true;
            m_queue.clear();
            m_queued_bytes = 0;
        }
    }
    // Owned exclusively by this thread from here: closing stdin is what
    // gives the child its EOF so it can finalize the MP4.
    close_stdin_handle();
}

std::string IsoFfmpegPipe::log_tail(size_t max_bytes) const
{
    FILE *f = nullptr;
#ifdef _WIN32
    _wfopen_s(&f, widen_utf8(m_log_path).c_str(), L"rb");
#else
    f = fopen(m_log_path.c_str(), "rb");
#endif
    if (!f)
        return {};
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long want = size > static_cast<long>(max_bytes)
                          ? static_cast<long>(max_bytes)
                          : size;
    std::string out(static_cast<size_t>(want < 0 ? 0 : want), '\0');
    if (want > 0) {
        fseek(f, size - want, SEEK_SET);
        const size_t got = fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    fclose(f);
    return out;
}
