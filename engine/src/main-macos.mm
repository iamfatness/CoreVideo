// ── macOS ZoomObsEngine — SCAFFOLD, NOT YET FUNCTIONAL ───────────────────────
//
// The Windows/Linux ZoomObsEngine (engine/src/main.cpp + engine-video.cpp /
// engine-share.cpp / engine-audio.cpp) is written entirely against the Zoom
// Meeting SDK's **C++** interface surface: <zoom_sdk.h>, the `ZOOMSDK`
// namespace, IAuthService / IMeetingService / IMeetingParticipantsController,
// SDKAuth(AuthContext), InitSDK(InitParam), the raw-recording + raw-data
// controllers, etc.
//
// The macOS Meeting SDK (ZoomSDK.framework, v7.1.5.84750) exposes **none** of
// that. It is a pure Objective-C framework: ZoomSDKAuthService,
// ZoomSDKMeetingService, ZoomSDKRawDataVideoSourceController,
// ZoomSDKRawDataAudioSourceController and Objective-C delegate protocols. There
// is no zoom_sdk.h and no ZOOMSDK:: C++ API anywhere in the archive. Porting the
// engine is therefore a full Objective-C++ rewrite, not a set of #ifdef fixes —
// it is tracked as an explicit gap in the macOS-port PR.
//
// This translation unit exists so that macOS CI compiles AND links a real
// ZoomObsEngine binary against the real ZoomSDK.framework — proving the SDK
// fetch, framework detection, header parsing, and link path end-to-end — WHILE
// REFUSING to fake any meeting functionality. It performs the platform-shared
// POSIX IPC handshake (identical wire protocol to the Windows/Linux engine),
// then, on any init/join request, reports a loud, explicit error over IPC and
// exits with a non-zero code. It NEVER silently pretends a meeting joined.
//
// When the Objective-C++ engine is written, it should replace this file (or be
// gated behind it) and implement the same IPC contract in engine-ipc.h.

#import <ZoomSDK/ZoomSDK.h>

#include "../../src/engine-ipc.h"
#include "engine-writer.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <string>

// ── POSIX IPC setup (mirrors engine/src/main.cpp's POSIX branch) ─────────────
static int unix_listen(const char *path)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0)
        return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path); // remove stale socket file
    if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 ||
        listen(srv, 1) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

static int unix_accept_timeout(int srv, int timeout_ms)
{
    struct pollfd pfd = {srv, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return -1;
    return accept(srv, nullptr, nullptr);
}

static bool ipc_setup(IpcFd &p2e, IpcFd &e2p)
{
    int srv_p2e = unix_listen(SOCK_P2E);
    int srv_e2p = unix_listen(SOCK_E2P);
    if (srv_p2e < 0 || srv_e2p < 0) {
        if (srv_p2e >= 0)
            close(srv_p2e);
        if (srv_e2p >= 0)
            close(srv_e2p);
        return false;
    }
    constexpr int kConnectTimeoutMs = 30000; // 30 s
    p2e = unix_accept_timeout(srv_p2e, kConnectTimeoutMs);
    e2p = unix_accept_timeout(srv_e2p, kConnectTimeoutMs);
    close(srv_p2e);
    close(srv_e2p);
    return p2e >= 0 && e2p >= 0;
}

static void ipc_teardown(IpcFd p2e, IpcFd e2p)
{
    close(p2e);
    close(e2p);
    unlink(SOCK_P2E);
    unlink(SOCK_E2P);
}

// Reference a real Objective-C class from ZoomSDK.framework so this binary is
// genuinely linked against the framework (forces resolution of the ObjC class
// symbol) and the framework's headers are exercised at compile time. This does
// NOT initialize the SDK, join a meeting, or start any Zoom activity.
static void link_check_zoom_framework(void)
{
    @autoreleasepool {
        ZoomSDKInitParams *params = [[ZoomSDKInitParams alloc] init];
        (void)params;
    }
}

int main()
{
    IpcFd p2e = kIpcInvalidFd;
    IpcFd e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p))
        return 1;
    EngineIpc::init(e2p); // must be called before any writes

    EngineIpc::write(R"({"cmd":"ready"})");
    link_check_zoom_framework();

    std::string line;
    while (ipc_read_line(p2e, line)) {
        if (line.empty())
            continue;

        if (line.find(IPC_CMD_QUIT) != std::string::npos)
            break;

        // Any real Zoom operation is unimplemented on macOS. Fail loudly with an
        // actionable message and a non-zero exit code so the plugin surfaces it
        // (ZoomEngineClient maps exit code 3 to RecoveryReason::SdkError) rather
        // than spinning as if a meeting were about to start.
        if (line.find(IPC_CMD_INIT) != std::string::npos ||
            line.find(IPC_CMD_JOIN) != std::string::npos) {
            EngineIpc::write(
                R"({"cmd":"auth_fail","stage":"macos_engine_unimplemented",)"
                R"("code":3,"name":"MACOS_ENGINE_NOT_IMPLEMENTED",)"
                R"("auth_mode":"unknown"})");
            EngineIpc::write(
                R"({"cmd":"error","msg":"macos_engine_unimplemented",)"
                R"("reason":"The macOS ZoomObsEngine is not yet implemented. The )"
                R"(macOS Zoom Meeting SDK is Objective-C only and requires an )"
                R"(Objective-C++ rewrite of the engine; see the CoreVideo macOS )"
                R"(port PR for the tracked gap."})");
            ipc_teardown(p2e, e2p);
            return 3; // SdkError — plugin treats this as a permanent SDK failure
        }
    }

    ipc_teardown(p2e, e2p);
    return 0;
}
