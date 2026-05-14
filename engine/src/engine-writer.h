#pragma once
// Thread-safe IPC write for the engine process.
// The Zoom SDK fires auth, meeting, participant, video, and audio callbacks on
// its own internal threads concurrently with the main IPC read loop.  All of
// them write to the same e2p fd — serialise those writes here.
#include "../../src/engine-ipc.h"
#include <mutex>
#include <string>

namespace EngineIpc {

inline IpcFd &fd()
{
    static IpcFd instance = kIpcInvalidFd;
    return instance;
}

inline std::mutex &mtx()
{
    static std::mutex instance;
    return instance;
}

// Call once from main() after e2p is established, before SDK callbacks start.
inline void init(IpcFd e2p) { fd() = e2p; }

// Serialised write — safe to call from any thread.
inline void write(const std::string &msg)
{
    std::lock_guard<std::mutex> lk(mtx());
    ipc_write_line(fd(), msg);
}

} // namespace EngineIpc
