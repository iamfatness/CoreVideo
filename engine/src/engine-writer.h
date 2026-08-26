#pragma once
// Thread-safe IPC write for the engine process.
// The Zoom SDK fires auth, meeting, participant, video, and audio callbacks on
// its own internal threads concurrently with the main IPC read loop.  All of
// them write to the same e2p fd — serialise those writes here.
#include "../../src/engine-ipc.h"
#include <functional>
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

// TEST-ONLY hook (Task 5 fix round 3, N6's engine-side pin). No production
// call site ever sets this -- main.cpp calls EngineIpc::init(), never this --
// so it stays nullptr and costs write() one branch on the real engine.
// tests/engine-talkback-select-test.cpp is the only caller: with no real e2p
// fd (CoreVideoEngineTalkbackSelectTest never calls init(), so fd() stays
// kIpcInvalidFd and ipc_write_line() below always fails harmlessly --
// ipc-hardening-test.cpp pins that), every report_nomination()/report()/
// report_session() line the engine emits was previously UNOBSERVABLE from a
// host test, which is how the fix for N1 could land on two of five
// structurally identical abort branches and 67/67 stayed green. Setting this
// is what makes the omission visible to a test at all.
inline std::function<void(const std::string &)> &test_sink()
{
    static std::function<void(const std::string &)> instance;
    return instance;
}

// Call once from main() after e2p is established, before SDK callbacks start.
inline void init(IpcFd e2p) { fd() = e2p; }

// Serialised write — safe to call from any thread. Returns false if the line
// could not be fully delivered (closed/broken/full pipe), so callers in the
// engine can stop emitting into a dead link.
inline bool write(const std::string &msg)
{
    std::lock_guard<std::mutex> lk(mtx());
    if (test_sink()) test_sink()(msg);
    return ipc_write_line(fd(), msg);
}

} // namespace EngineIpc
