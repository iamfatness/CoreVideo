#include "../../src/engine-ipc.h"
#include "engine-video.h"
#include "engine-audio.h"
#include <windows.h>
#include <string>
#include <atomic>
static std::string read_line(HANDLE pipe)
{
    std::string line; char ch; DWORD n;
    while (ReadFile(pipe, &ch, 1, &n, nullptr) && n == 1) { if (ch == '\n') break; line += ch; }
    return line;
}
static void write_line(HANDLE pipe, const std::string &msg)
{
    std::string out = msg + "\n"; DWORD written;
    WriteFile(pipe, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
}
int main()
{
    HANDLE p2e = CreateNamedPipeA(PIPE_P2E, PIPE_ACCESS_INBOUND,  PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    HANDLE e2p = CreateNamedPipeA(PIPE_E2P, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    ConnectNamedPipe(p2e, nullptr);
    ConnectNamedPipe(e2p, nullptr);
    write_line(e2p, R"({"cmd":"ready"})");
    std::atomic<bool> running{true};
    while (running) {
        std::string line = read_line(p2e);
        if (line.empty()) continue;
        if      (line.find(IPC_CMD_QUIT)        != std::string::npos) { running = false; }
        else if (line.find(IPC_CMD_INIT)        != std::string::npos) { write_line(e2p, R"({"cmd":"auth_ok"})"); }
        else if (line.find(IPC_CMD_JOIN)        != std::string::npos) { write_line(e2p, R"({"cmd":"joined","meeting_id":""})"); }
        else if (line.find(IPC_CMD_LEAVE)       != std::string::npos) { write_line(e2p, R"({"cmd":"left"})"); }
        else if (line.find(IPC_CMD_SUBSCRIBE)   != std::string::npos) { /* EngineVideo::subscribe(...) */ }
        else if (line.find(IPC_CMD_UNSUBSCRIBE) != std::string::npos) { /* EngineVideo::unsubscribe(...) */ }
    }
    CloseHandle(p2e);
    CloseHandle(e2p);
    return 0;
}
