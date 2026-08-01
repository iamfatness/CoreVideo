// ── macOS ZoomObsEngine — Objective-C++ implementation ───────────────────────
//
// The Windows/Linux ZoomObsEngine (engine/src/main.cpp + engine-video.cpp /
// engine-share.cpp / engine-audio.cpp) is written entirely against the Zoom
// Meeting SDK's **C++** interface surface: <zoom_sdk.h>, the `ZOOMSDK`
// namespace, IAuthService / IMeetingService / IMeetingParticipantsController,
// SDKAuth(AuthContext), InitSDK(InitParam), the raw-data controllers, etc.
//
// The macOS Meeting SDK (ZoomSDK.framework, v7.1.5.84750) exposes **none** of
// that. It is a pure Objective-C framework: ZoomSDKAuthService,
// ZoomSDKMeetingService, ZoomSDKRawDataVideoSourceController,
// ZoomSDKRawDataAudioSourceController and Objective-C delegate protocols. So
// this file is a full Objective-C++ rewrite rather than a set of #ifdef fixes,
// implementing the SAME IPC wire protocol defined in src/engine-ipc.h.
//
// ── Threading (the constraint that shapes this file) ─────────────────────────
// The macOS SDK delivers every result through Objective-C delegate callbacks
// dispatched on the main run loop — the SDK's own sample app is a plain Cocoa
// app built on NSApplicationMain. The previous scaffold blocked the main thread
// in `while (ipc_read_line(...))`, which would starve that run loop, so no
// delegate could ever fire and auth would hang forever with no error.
//
// Therefore: the main thread runs the Cocoa run loop and nothing else, the IPC
// read loop runs on a background thread, and every SDK call is hopped back onto
// the main queue. Delegate callbacks then arrive on the main thread and write
// to IPC from there. EngineIpc::write is serialized with its own mutex, so
// interleaved writes from the reader thread and the main thread stay
// line-atomic.
//
// ── The SDK runtime must live in the app bundle (hard-won) ───────────────────
// ZoomSDK.framework is not self-contained. At auth time the SDK loads a set of
// sibling *bundles* (ssb_sdk, zNet, zPTUIEx, ZoomSDKChatUI, ...) and it finds
// them through the MAIN BUNDLE's Frameworks directory — NOT through the linker's
// rpath and NOT relative to ZoomSDK.framework's own location. Linking and
// loading the framework therefore proves nothing about whether auth can work.
//
// When those bundles are missing the failure is silent and misleading:
// initSDKWithParams still returns Success, getAuthService still returns a live
// object, and then sdkAuth returns ZoomSDKError_Failed(1) *synchronously* with
// no delegate callback ever firing — because the internal auth manager has no
// web-service module to hand the request to. Disassembling -[ZoomSDKAuthService
// sdkAuth:] confirms the auth context itself parses fine (an empty context
// returns InvalidParameter(5), which we never saw); the Failed(1) comes from an
// internal manager returning false before anything reaches the network.
//
// So the engine must ship as ZoomObsEngine.app with the SDK runtime in
// Contents/Frameworks (see scripts/make-macos-bundle.sh). preflight_sdk_runtime
// below turns that requirement into one explicit IPC error instead of a
// synchronous auth failure with no explanation.
//
// ── Implemented so far ───────────────────────────────────────────────────────
//   init  -> SDK init + sdkAuth, emitting auth_ok / auth_fail
// Everything else (join, roster, raw media) still fails loudly over IPC rather
// than pretending, exactly as before. Do NOT fake frames.

#import <ZoomSDK/ZoomSDK.h>
#import <Cocoa/Cocoa.h>

#include "../../src/engine-ipc.h"
#include "engine-writer.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>

// ── JSON helpers ─────────────────────────────────────────────────────────────
// Deliberately identical to the ones in main.cpp so both engines parse the
// plugin's messages the same way. The plugin emits flat, well-formed objects;
// this is the same intentionally-minimal scanner the Windows engine uses.
static std::string json_str(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\') {
            if (pos < json.size()) pos++; // skip escaped character
            continue;
        }
        if (c == '"') break;
        result += c;
    }
    return result;
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

static std::string redacted_tail(const std::string &value)
{
    if (value.empty()) return "empty";
    if (value.size() <= 4) return "****";
    return "****" + value.substr(value.size() - 4);
}

// ── Auth result naming ───────────────────────────────────────────────────────
// The plugin classifies auth failures by NAME, not by numeric code:
// zoom_join::classify_sdk_auth_result() in src/zoom-join-decision.h switches on
// the AUTHRET_* strings the Windows engine reports, and the code is only echoed
// into the operator message for support bundles. The macOS ZoomSDKAuthError
// enum is a different type with different numeric values, so mapping it onto
// the same AUTHRET_* vocabulary is what makes the plugin's existing error
// catalog work unchanged on macOS. Do not "simplify" this into the raw ObjC
// enum name -- that would silently degrade every auth error to the generic
// fallback message.
static const char *auth_result_name(ZoomSDKAuthError ret)
{
    switch (ret) {
    case ZoomSDKAuthError_Success:                return "AUTHRET_SUCCESS";
    case ZoomSDKAuthError_KeyOrSecretWrong:       return "AUTHRET_KEYORSECRETWRONG";
    case ZoomSDKAuthError_AccountNotSupport:      return "AUTHRET_ACCOUNTNOTSUPPORT";
    case ZoomSDKAuthError_AccountNotEnableSDK:    return "AUTHRET_ACCOUNTNOTENABLESDK";
    case ZoomSDKAuthError_Timeout:                return "AUTHRET_OVERTIME";
    case ZoomSDKAuthError_NetworkIssue:           return "AUTHRET_NETWORKISSUE";
    case ZoomSDKAuthError_Client_Incompatible:    return "AUTHRET_CLIENT_INCOMPATIBLE";
    case ZoomSDKAuthError_JwtTokenWrong:          return "AUTHRET_JWTTOKENWRONG";
    case ZoomSDKAuthError_KeyOrSecretEmpty:       return "AUTHRET_KEYORSECRETEMPTY";
    case ZoomSDKAuthError_LimitExceededException: return "AUTHRET_LIMIT_EXCEEDED_EXCEPTION";
    case ZoomSDKAuthError_Unknown:                return "AUTHRET_UNKNOWN";
    default:                                      return "AUTHRET_UNKNOWN";
    }
}

// Auth mode of the in-flight sdkAuth call ("jwt" or "public_app_key"). The
// plugin needs it to interpret a key/secret/jwt rejection correctly. Written on
// the main queue before sdkAuth and read in the delegate callback, which also
// runs on the main queue, so no additional synchronization is needed.
static std::string g_current_auth_mode = "jwt";

// ── Auth delegate ────────────────────────────────────────────────────────────
@interface CVAuthDelegate : NSObject <ZoomSDKAuthDelegate>
@end

@implementation CVAuthDelegate

- (void)onZoomSDKAuthReturn:(ZoomSDKAuthError)returnValue
{
    if (returnValue == ZoomSDKAuthError_Success) {
        EngineIpc::write(R"({"cmd":"auth_ok"})");
        return;
    }
    EngineIpc::write(std::string(R"({"cmd":"auth_fail","code":)") +
                     std::to_string(static_cast<int>(returnValue)) +
                     R"(,"name":")" + auth_result_name(returnValue) +
                     R"(","auth_mode":")" + json_escape(g_current_auth_mode) +
                     "\"}");
}

- (void)onZoomAuthIdentityExpired
{
    EngineIpc::write(R"({"cmd":"error","msg":"identity_expired"})");
}

- (void)onZoomIdentityExpired
{
    EngineIpc::write(R"({"cmd":"error","msg":"identity_expired"})");
}

@end

static CVAuthDelegate *g_auth_delegate = nil;

// ── SDK runtime preflight ────────────────────────────────────────────────────
// See the header note: the SDK resolves its runtime bundles through the main
// bundle's Frameworks directory, and their absence surfaces only as a
// synchronous sdkAuth failure with no callback. Check it up front so the cause
// is stated instead of inferred. Returns the directory it checked so the error
// can name it.
static bool preflight_sdk_runtime(std::string &frameworks_dir)
{
    NSString *frameworks = [[NSBundle mainBundle] privateFrameworksPath];
    if (!frameworks) {
        frameworks_dir = "(no main bundle: the engine is not running from a .app)";
        return false;
    }
    frameworks_dir = frameworks.UTF8String ? frameworks.UTF8String : "";
    NSString *sdk = [frameworks stringByAppendingPathComponent:@"ZoomSDK.framework"];
    return [[NSFileManager defaultManager] fileExistsAtPath:sdk];
}

// ── init / auth ──────────────────────────────────────────────────────────────
// Runs on the main queue. Mirrors the init branch of main.cpp's command loop,
// including the debug breadcrumbs, which are what make a hung auth diagnosable.
static void handle_init(const std::string &line)
{
    std::string jwt = json_str(line, "jwt");
    const std::string public_app_key = json_str(line, "public_app_key");
    EngineIpc::write(R"({"cmd":"debug","stage":"init_received"})");

    std::string frameworks_dir;
    if (!preflight_sdk_runtime(frameworks_dir)) {
        EngineIpc::write(
            std::string(R"({"cmd":"auth_fail","stage":"sdk_runtime_missing","code":0,)"
                        R"("name":"AUTHRET_UNKNOWN","auth_mode":")") +
            json_escape(public_app_key.empty() ? "jwt" : "public_app_key") +
            R"(","reason":"ZoomSDK.framework is not in the engine app's )"
            R"(Frameworks directory. The macOS Zoom SDK loads its runtime )"
            R"(bundles from there, so authentication cannot work without it. )"
            R"(Rebuild the bundle with scripts/make-macos-bundle.sh. Looked in: )" +
            json_escape(frameworks_dir) + "\"}");
        return;
    }

    ZoomSDK *sdk = [ZoomSDK sharedSDK];

    ZoomSDKInitParams *params = [[ZoomSDKInitParams alloc] init];
    params.needCustomizedUI = YES;   // headless helper: never show Zoom's own UI
    params.enableLog = YES;
    params.zoomDomain = @"https://zoom.us";

    // Raw-data memory mode is configured on the ZoomSDK singleton, NOT on
    // ZoomSDKInitParams (unlike Windows, where it lives in InitParam.rawdataOpts).
    // Heap mode matches the Windows engine's ZoomSDKRawDataMemoryModeHeap so the
    // SHM writer sees the same buffer lifetime rules once raw media lands.
    sdk.videoRawDataMode = ZoomSDKRawDataMemoryMode_Heap;
    sdk.shareRawDataMode = ZoomSDKRawDataMemoryMode_Heap;
    sdk.audioRawDataMode = ZoomSDKRawDataMemoryMode_Heap;

    EngineIpc::write(R"({"cmd":"debug","stage":"before_init_sdk"})");
    ZoomSDKError err = [sdk initSDKWithParams:params];
    EngineIpc::write(R"({"cmd":"debug","stage":"after_init_sdk","code":)" +
                     std::to_string(static_cast<int>(err)) + "}");
    if (err != ZoomSDKError_Success) {
        EngineIpc::write(R"({"cmd":"auth_fail","stage":"init","code":)" +
                         std::to_string(static_cast<int>(err)) +
                         R"(,"name":"AUTHRET_UNKNOWN","auth_mode":")" +
                         json_escape(public_app_key.empty() ? "jwt"
                                                            : "public_app_key") +
                         "\"}");
        return;
    }

    EngineIpc::write(R"({"cmd":"debug","stage":"before_create_auth"})");
    ZoomSDKAuthService *auth_svc = [sdk getAuthService];
    if (!auth_svc) {
        EngineIpc::write(R"({"cmd":"auth_fail","stage":"create_auth","code":0,)"
                         R"("name":"AUTHRET_UNKNOWN","auth_mode":")" +
                         json_escape(public_app_key.empty() ? "jwt"
                                                            : "public_app_key") +
                         "\"}");
        return;
    }

    // The delegate is an assign (unowned) property, so this object must outlive
    // the service; it is intentionally a never-released global.
    if (!g_auth_delegate) g_auth_delegate = [[CVAuthDelegate alloc] init];
    auth_svc.delegate = g_auth_delegate;

    ZoomSDKAuthContext *ctx = [[ZoomSDKAuthContext alloc] init];
    if (!public_app_key.empty()) {
        // public_app_key and jwt are mutually exclusive; sending both lets the
        // SDK pick, which makes failures impossible to attribute.
        jwt.clear();
        ctx.publicAppKey = [NSString stringWithUTF8String:public_app_key.c_str()];
        ctx.jwtToken = nil;
        g_current_auth_mode = "public_app_key";
    } else {
        ctx.jwtToken = [NSString stringWithUTF8String:jwt.c_str()];
        ctx.publicAppKey = nil;
        g_current_auth_mode = "jwt";
    }

    EngineIpc::write(
        R"({"cmd":"debug","stage":"before_sdk_auth","auth_mode":")" +
        std::string(public_app_key.empty() ? "jwt" : "public_app_key") +
        R"(","jwt_present":)" + std::string(jwt.empty() ? "false" : "true") +
        R"(,"public_app_key_present":)" +
        std::string(public_app_key.empty() ? "false" : "true") +
        R"(,"public_app_key_tail":")" +
        json_escape(redacted_tail(public_app_key)) + "\"" + "}");

    err = [auth_svc sdkAuth:ctx];
    EngineIpc::write(R"({"cmd":"debug","stage":"after_sdk_auth","code":)" +
                     std::to_string(static_cast<int>(err)) + "}");
    if (err != ZoomSDKError_Success) {
        // Synchronous rejection: onZoomSDKAuthReturn will never fire, so report
        // here or the plugin would wait on a callback that is not coming.
        EngineIpc::write(R"({"cmd":"auth_fail","stage":"sdk_auth","code":)" +
                         std::to_string(static_cast<int>(err)) +
                         R"(,"name":")" + auth_result_name(ZoomSDKAuthError_Unknown) +
                         R"(","auth_mode":")" + json_escape(g_current_auth_mode) +
                         "\"}");
    }
}

// Anything not yet ported fails loudly and never silently pretends.
static void handle_unimplemented(const char *stage)
{
    EngineIpc::write(
        std::string(R"({"cmd":"error","msg":"macos_engine_unimplemented",)"
                    R"("stage":")") + stage + R"(",)"
        R"("reason":"This macOS ZoomObsEngine path is not implemented yet. )"
        R"(SDK init and authentication are ported; join, roster and raw media )"
        R"(are still in progress. See the CoreVideo macOS port PR."})");
}

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

int main()
{
    IpcFd p2e = kIpcInvalidFd;
    IpcFd e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p))
        return 1;
    EngineIpc::init(e2p); // must be called before any writes

    EngineIpc::write(R"({"cmd":"ready"})");

    // The IPC read loop must NOT run on the main thread: the main thread has to
    // stay in the Cocoa run loop so the SDK's delegate callbacks can be
    // delivered. Reading here and dispatching SDK work to the main queue is what
    // makes asynchronous auth possible at all.
    std::thread reader([p2e, e2p]() {
        std::string line;
        while (ipc_read_line(p2e, line)) {
            if (line.empty())
                continue;

            if (line.find(IPC_CMD_QUIT) != std::string::npos)
                break;

            // Copy for the block: `line` is reused by the next read.
            const std::string msg = line;
            if (msg.find(IPC_CMD_INIT) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{ handle_init(msg); });
            } else if (msg.find(IPC_CMD_JOIN) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(),
                               ^{ handle_unimplemented("join"); });
            }
        }

        // Peer closed the socket or sent quit: unwind from the main thread,
        // which owns the run loop.
        dispatch_async(dispatch_get_main_queue(), ^{
            ipc_teardown(p2e, e2p);
            [[ZoomSDK sharedSDK] unInitSDK];
            exit(0);
        });
    });
    reader.detach();

    // Hand the main thread to Cocoa. Delegate callbacks are delivered from here;
    // the reader thread's final dispatch_async is what ends the process.
    //
    // A bare [[NSRunLoop currentRunLoop] run] is NOT sufficient: the SDK is an
    // AppKit client (its own sample is built on NSApplicationMain), so an
    // NSApplication instance must exist. Accessory activation policy keeps this
    // helper out of the Dock and gives it no menu bar, which is what we want for
    // a process OBS launches in the background.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp run];
    return 0;
}
