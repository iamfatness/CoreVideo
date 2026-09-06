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
#include "../../src/shm-generation.h"
#include "../../src/raw-media-lifecycle.h"
#include "engine-writer.h"
#include "tile-clock-log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// Defined with the raw-media code further down; the meeting delegate has to
// release renderers and SHM regions when the meeting ends, and attach the
// screen-share controller once the meeting is up.
static void handle_stop_media(const char *reason);
static void raw_media_event(RawMediaEvent event, const char *reason, int sdk_code = 0);
static void share_attach();
static void share_teardown();

// Set false during teardown so the heartbeat thread stops before the IPC
// sockets close. File scope because the heartbeat thread is detached and
// outlives any narrower scope.
static std::atomic<bool> g_running{true};

// False until initSDKWithParams returns. That call legitimately blocks the
// main thread for 10-15s (longer after a killed engine), and pings routed
// through the blocked main queue made the plugin's 10s watchdog kill every
// join at "before_init_sdk". Until init completes the heartbeat thread writes
// pings directly (same blind spot the pre-main-queue heartbeat always had);
// afterwards pings go via the main queue so a hung main thread is detected.
static std::atomic<bool> g_heartbeat_via_main_queue{false};

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
    // Match the Windows engine, which leaves InitParam's customized-UI flag at
    // its default and therefore runs with the SDK's own UI available. An earlier
    // revision of this file forced customized UI on "headless helper" grounds;
    // that was a macOS-only divergence with no counterpart on Windows, and it
    // removed the operator's only view of what the engine is actually sending
    // back into the meeting.
    params.needCustomizedUI = NO;
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
    // SDK init is done — from here on, heartbeat pings must prove the main
    // thread is alive (see the heartbeat thread in main()).
    g_heartbeat_via_main_queue.store(true, std::memory_order_release);
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

// ── Meeting failure names ────────────────────────────────────────────────────
// Keyed on the raw integer rather than the ObjC enum on purpose. The plugin
// classifies join failures by NUMERIC CODE (zoom_join::classify_join_failure in
// src/zoom-join-decision.h switches on 23/60/62/63/64/82/500..506), and those
// numbers are identical in Windows' MeetingFailCode and macOS'
// ZoomSDKMeetingError — only the spellings differ. So the code passes through
// untouched and this table only supplies the human-readable name for operator
// messages and support bundles, in the same MEETING_FAIL_* vocabulary the
// Windows engine emits. Do NOT switch on the ObjC enum names here: that would
// read as a translation and invite someone to "fix" the codes to match it.
static const char *meeting_fail_name(int code)
{
    switch (code) {
    case 1:   return "MEETING_FAIL_CONNECTION_ERR";
    case 2:   return "MEETING_FAIL_RECONNECT_ERR";
    case 3:   return "MEETING_FAIL_MMR_ERR";
    case 4:   return "MEETING_FAIL_PASSWORD_ERR";
    case 5:   return "MEETING_FAIL_SESSION_ERR";
    case 6:   return "MEETING_FAIL_MEETING_OVER";
    case 7:   return "MEETING_FAIL_MEETING_NOT_START";
    case 8:   return "MEETING_FAIL_MEETING_NOT_EXIST";
    case 9:   return "MEETING_FAIL_MEETING_USER_FULL";
    case 10:  return "MEETING_FAIL_CLIENT_INCOMPATIBLE";
    case 11:  return "MEETING_FAIL_NO_MMR";
    case 12:  return "MEETING_FAIL_CONFLOCKED";
    case 13:  return "MEETING_FAIL_MEETING_RESTRICTED";
    case 14:  return "MEETING_FAIL_MEETING_RESTRICTED_JBH";
    case 15:  return "MEETING_FAIL_CANNOT_EMIT_WEBREQUEST";
    case 16:  return "MEETING_FAIL_CANNOT_START_TOKENEXPIRE";
    case 19:  return "MEETING_FAIL_REGISTERWEBINAR_FULL";
    case 20:  return "MEETING_FAIL_REGISTERWEBINAR_HOSTREGISTER";
    case 21:  return "MEETING_FAIL_REGISTERWEBINAR_PANELISTREGISTER";
    case 22:  return "MEETING_FAIL_REGISTERWEBINAR_DENIED_EMAIL";
    case 23:  return "MEETING_FAIL_ENFORCE_LOGIN";
    case 50:  return "MEETING_FAIL_WRITE_CONFIG_FILE";
    case 60:  return "MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING";
    case 62:  return "MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN";
    case 63:  return "MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING";
    case 64:  return "MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN";
    case 82:  return "MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING";
    case 500: return "MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR";
    case 501: return "MEETING_FAIL_AUTHORIZED_USER_NOT_INMEETING";
    case 502: return "MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR";
    case 503: return "MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF";
    case 504: return "MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING";
    case 505: return "MEETING_FAIL_ON_BEHALF_TOKEN_INVALID";
    case 506: return "MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING";
    default:  return "MEETING_FAIL_UNKNOWN";
    }
}

// ── Roster state ─────────────────────────────────────────────────────────────
// Mirrors ParticipantInfo in engine/src/main.cpp so `participants` is emitted
// byte for byte identically — the plugin's read side is shared and unchanged.
//
// The Windows engine guards this state with a mutex because the SDK fires its
// callbacks from arbitrary threads. Here everything — delegate callbacks and
// every command hopped over from the reader thread — runs on the main queue, so
// there is nothing to race with. That invariant is load-bearing: never touch
// g_roster from the reader thread.
struct ParticipantInfo {
    uint32_t user_id = 0;
    std::string display_name;
    bool has_video = false;
    bool is_talking = false;
    bool is_muted = false;
    bool is_sharing_screen = false;
};

static std::vector<ParticipantInfo> g_roster;
static uint32_t g_active_speaker = 0;

// Who is currently screen sharing, or 0. Written by the share delegate (main
// queue) and read by user_to_info, which also runs there; atomic because the
// share renderer's frame callback reads it from an SDK thread.
static std::atomic<uint32_t> g_active_share_user{0};

static ZoomSDKMeetingService *meeting_service()
{
    return [[ZoomSDK sharedSDK] getMeetingService];
}

static ZoomSDKMeetingActionController *action_controller()
{
    ZoomSDKMeetingService *svc = meeting_service();
    return svc ? [svc getMeetingActionController] : nil;
}

static std::string to_utf8(NSString *s)
{
    if (!s) return {};
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

static ParticipantInfo user_to_info(ZoomSDKUserInfo *u)
{
    ParticipantInfo info;
    if (!u) return info;
    info.user_id      = [u getUserID];
    info.display_name = to_utf8([u getUserName]);
    info.has_video    = [u isVideoOn] ? true : false;
    info.is_talking   = [u isTalking] ? true : false;

    // Windows reads IsAudioMuted() directly; macOS only exposes a status enum,
    // so fold the three muted variants onto the same boolean.
    const ZoomSDKAudioStatus audio = [u getAudioStatus];
    info.is_muted = (audio == ZoomSDKAudioStatus_Muted ||
                     audio == ZoomSDKAudioStatus_MutedByHost ||
                     audio == ZoomSDKAudioStatus_MutedAllByHost);

    info.is_sharing_screen =
        info.user_id == g_active_share_user.load(std::memory_order_acquire);
    return info;
}

static void rebuild_roster()
{
    ZoomSDKMeetingActionController *ctrl = action_controller();
    if (!ctrl) return;
    NSArray *list = [ctrl getParticipantsList];
    if (!list) return;

    std::vector<ParticipantInfo> next;
    next.reserve(list.count);
    for (NSNumber *uid in list) {
        ZoomSDKUserInfo *user = [ctrl getUserByUserID:uid.unsignedIntValue];
        if (!user) continue;
        next.push_back(user_to_info(user));
    }
    g_roster = std::move(next);

    g_active_speaker = 0;
    for (const auto &p : g_roster) {
        if (p.is_talking) {
            g_active_speaker = p.user_id;
            break;
        }
    }
}

static void send_roster()
{
    std::string msg = R"({"cmd":"participants","active_speaker_id":)" +
        std::to_string(g_active_speaker) + R"(,"participants":[)";
    for (size_t i = 0; i < g_roster.size(); ++i) {
        const auto &p = g_roster[i];
        if (i) msg += ",";
        msg += R"({"id":)" + std::to_string(p.user_id) +
            R"(,"name":")" + json_escape(p.display_name) +
            R"(","has_video":)" + (p.has_video ? "true" : "false") +
            R"(,"is_talking":)" + (p.is_talking ? "true" : "false") +
            R"(,"is_muted":)" + (p.is_muted ? "true" : "false") +
            R"(,"is_sharing_screen":)" +
            (p.is_sharing_screen ? "true" : "false") + "}";
    }
    msg += "]}";
    EngineIpc::write(msg);
}

// ── Meeting + roster delegate ────────────────────────────────────────────────
// Reproduces onMeetingStatusChanged / the participants-controller callbacks from
// engine/src/main.cpp. Every method of ZoomSDKMeetingActionControllerDelegate is
// @required (the protocol declares no @optional section), so the ones we do not
// use are stubbed rather than omitted — same shape as the Windows engine's block
// of empty overrides. An omitted @required method is an unrecognized-selector
// crash the moment the SDK calls it.
@interface CVMeetingDelegate : NSObject <ZoomSDKMeetingServiceDelegate,
                                         ZoomSDKMeetingActionControllerDelegate>
@end

@implementation CVMeetingDelegate

- (void)onMeetingStatusChange:(ZoomSDKMeetingStatus)state
                 meetingError:(ZoomSDKMeetingError)error
                    EndReason:(EndMeetingReason)reason
{
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self onMeetingStatusChange:state meetingError:error EndReason:reason];
        });
        return;
    }
    EngineIpc::write(R"({"cmd":"debug","stage":"meeting_status","status":)" +
                     std::to_string(static_cast<int>(state)) +
                     R"(,"result":)" + std::to_string(static_cast<int>(error)) + "}");

    switch (state) {
    case ZoomSDKMeetingStatus_InMeeting: {
        EngineIpc::write(R"({"cmd":"joined"})");
        ZoomSDKMeetingActionController *ctrl = action_controller();
        if (!ctrl) {
            // Without it there is no roster at all (getParticipantsList and
            // getUserByUserID both live here), so say so rather than silently
            // reporting an empty meeting.
            EngineIpc::write(
                R"({"cmd":"debug","stage":"participants_controller","code":-1})");
            break;
        }
        ctrl.delegate = self;
        share_attach();
        rebuild_roster();
        send_roster();
        raw_media_event(RawMediaEvent::InMeeting, "in_meeting");
        break;
    }
    case ZoomSDKMeetingStatus_Join_Breakout_Room:
    case ZoomSDKMeetingStatus_Leave_Breakout_Room:
        raw_media_event(RawMediaEvent::Transition, "breakout_transition");
        break;
    case ZoomSDKMeetingStatus_Disconnecting:
    case ZoomSDKMeetingStatus_Ended: {
        handle_stop_media("meeting_left");
        ZoomSDKMeetingActionController *ctrl = action_controller();
        if (ctrl && ctrl.delegate == self) ctrl.delegate = nil;
        g_roster.clear();
        g_active_speaker = 0;
        EngineIpc::write(R"({"cmd":"left"})");
        break;
    }
    case ZoomSDKMeetingStatus_Failed: {
        handle_stop_media("meeting_failed");
        ZoomSDKMeetingActionController *ctrl = action_controller();
        if (ctrl && ctrl.delegate == self) ctrl.delegate = nil;
        g_roster.clear();
        g_active_speaker = 0;
        EngineIpc::write(R"({"cmd":"error","msg":"meeting_failed","code":)" +
                         std::to_string(static_cast<int>(error)) +
                         R"(,"reason":")" +
                         meeting_fail_name(static_cast<int>(error)) + "\"}");
        break;
    }
    default:
        break;
    }
}

// ── Roster-affecting callbacks (the ones main.cpp acts on) ───────────────────
- (void)onUserJoin:(NSArray *)array { rebuild_roster(); send_roster(); }
- (void)onUserLeft:(NSArray *)array { rebuild_roster(); send_roster(); }
- (void)onUserNamesChanged:(NSArray<NSNumber *> *)userList { rebuild_roster(); send_roster(); }
- (void)onUserAudioStatusChange:(NSArray *)userAudioStatusArray { rebuild_roster(); send_roster(); }
- (void)onVideoStatusChange:(ZoomSDKVideoStatus)videoStatus UserID:(unsigned int)userID
{
    rebuild_roster();
    send_roster();
}

- (void)onUserActiveAudioChange:(NSArray *)useridArray
{
    // main.cpp's onUserActiveAudioChange: the head of the list is the active
    // speaker, and every id in it is currently talking.
    const uint32_t active =
        (useridArray.count > 0)
            ? [(NSNumber *)useridArray.firstObject unsignedIntValue]
            : 0;
    g_active_speaker = active;
    for (auto &p : g_roster) p.is_talking = false;
    for (NSNumber *uid in useridArray) {
        const uint32_t id = uid.unsignedIntValue;
        for (auto &p : g_roster) {
            if (p.user_id == id) p.is_talking = true;
        }
    }
    EngineIpc::write(R"({"cmd":"active_speaker","participant_id":)" +
                     std::to_string(active) + "}");
    send_roster();
}

- (void)onActiveSpeakerVideoUserChanged:(unsigned int)userID
{
    if (g_active_speaker == 0) g_active_speaker = userID;
    EngineIpc::write(R"({"cmd":"active_speaker","participant_id":)" +
                     std::to_string(userID) + "}");
}

// ── Required-but-unused (see the class comment) ──────────────────────────────
- (void)onUserInfoUpdate:(unsigned int)userID {}
- (void)onVirtualNameTagStatusChanged:(BOOL)bOn userID:(unsigned int)userID {}
- (void)onVirtualNameTagRosterInfoUpdated:(unsigned int)userID {}
- (void)onHostChange:(unsigned int)userID {}
- (void)onMeetingCoHostChanged:(unsigned int)userID isCoHost:(BOOL)isCoHost {}
- (void)onSpotlightVideoUserChange:(NSArray *)spotlightedUserList {}
- (void)onLowOrRaiseHandStatusChange:(BOOL)raise UserID:(unsigned int)userID {}
- (void)onJoinMeetingResponse:(ZoomSDKJoinMeetingHelper *)joinMeetingHelper {}
- (void)onMultiToSingleShareNeedConfirm:(ZoomSDKMultiToSingleShareConfirmHandler *)confirmHandle {}
- (void)onActiveVideoUserChanged:(unsigned int)userID {}
- (void)onHostAskUnmute {}
- (void)onHostAskStartVideo {}
- (void)onInvalidReclaimHostKey {}
- (void)onHostVideoOrderUpdated:(NSArray *)orderList {}
- (void)onLocalVideoOrderUpdated:(NSArray *)localOrderList {}
- (void)onFollowHostVideoOrderChanged:(BOOL)follow {}
- (void)onAllHandsLowered {}
- (void)onUserVideoQualityChanged:(ZoomSDKVideoQuality)quality userID:(unsigned int)userID {}
- (void)onChatMsgDeleteNotification:(NSString *)msgID
                  messageDeleteType:(ZoomSDKChatMessageDeleteType)deleteBy {}
- (void)onChatStatusChangedNotification:(ZoomSDKChatStatus *)chatStatus {}
- (void)onShareMeetingChatStatusChanged:(BOOL)isStart {}
- (void)onSuspendParticipantsActivities {}
- (void)onAllowParticipantsStartVideoNotification:(BOOL)allow {}
- (void)onAllowParticipantsRenameNotification:(BOOL)allow {}
- (void)onAllowParticipantsUnmuteSelfNotification:(BOOL)allow {}
- (void)onAllowParticipantsShareWhiteBoardNotification:(BOOL)allow {}
- (void)onMeetingLockStatus:(BOOL)isLock {}
- (void)onRequestLocalRecordingPrivilegeChanged:(ZoomSDKLocalRecordingRequestPrivilegeStatus)status {}
- (void)onAllowParticipantsRequestCloudRecording:(BOOL)allow {}
- (void)onInMeetingUserAvatarPathUpdated:(unsigned int)userID {}
- (void)onAICompanionActiveChangeNotice:(BOOL)active {}
- (void)onParticipantProfilePictureStatusChange:(BOOL)hidden {}
- (void)onVideoAlphaChannelStatusChanged:(BOOL)isAlphaModeOn {}
- (void)onFocusModeStateChanged:(BOOL)on {}
- (void)onFocusModeShareTypeChanged:(ZoomSDKFocusModeShareType)shareType {}
- (void)onMeetingQAStatusChanged:(BOOL)isMeetingQAFeatureOn {}
- (void)onCameraControlRequestReceived:(unsigned int)userId
                           requestType:(ZoomSDKCameraControlRequestType)requestType
                         actionApprove:(ZoomSDKError (^)(void))actionApprove
                         actionDecline:(ZoomSDKError (^)(void))actionDecline {}
- (void)onCameraControlRequestResult:(unsigned int)userId
                          resultType:(ZoomSDKCameraControlRequestResult)resultType {}
- (void)onMuteOnEntryStatusChange:(BOOL)enable {}
- (void)onMeetingTopicChanged:(NSString *)topic {}
- (void)onBotAuthorizerRelationChanged:(unsigned int)authorizeUserID {}
- (void)onCreateCompanionRelation:(unsigned int)parentUserID
                      childUserID:(unsigned int)childUserID {}
- (void)onRemoveCompanionRelation:(unsigned int)childUserID {}
- (void)onGrantCoOwnerPrivilegeChanged:(BOOL)canGrantOther {}
- (void)notifyToJoin3rdPartyTelephonyAudio:(NSString *)audioInfo {}

@end

static CVMeetingDelegate *g_meeting_delegate = nil;

// The join context is deliberately a never-released global. main.cpp keeps its
// JoinParam strings alive for the same reason: Join/joinMeeting: is asynchronous
// and this file is built without ARC, so releasing the context on return would
// risk pulling it out from under an in-flight call.
static ZoomSDKJoinMeetingElements *g_join_ctx = nil;

static NSString *ns_or_nil(const std::string &s)
{
    return s.empty() ? nil : [NSString stringWithUTF8String:s.c_str()];
}

// ── join / leave ─────────────────────────────────────────────────────────────
// Runs on the main queue. Mirrors the join branch of main.cpp's command loop,
// including the breadcrumb shape, so the two engines are diffable in a log.
static void handle_join(const std::string &line)
{
    const std::string meeting_id          = json_str(line, "meeting_id");
    const std::string passcode            = json_str(line, "passcode");
    std::string       display_name        = json_str(line, "display_name");
    const std::string on_behalf_token     = json_str(line, "on_behalf_token");
    const std::string user_zak            = json_str(line, "user_zak");
    const std::string app_privilege_token = json_str(line, "app_privilege_token");
    if (display_name.empty()) display_name = "OBS";

    EngineIpc::write(R"({"cmd":"debug","stage":"join_received","meeting_id":")" +
        json_escape(meeting_id) + R"(","has_on_behalf_token":)" +
        std::string(on_behalf_token.empty() ? "false" : "true") +
        R"(,"has_user_zak":)" +
        std::string(user_zak.empty() ? "false" : "true") +
        R"(,"has_app_privilege_token":)" +
        std::string(app_privilege_token.empty() ? "false" : "true") + "}");

    ZoomSDKMeetingService *svc = meeting_service();
    if (!svc) {
        EngineIpc::write(
            R"({"cmd":"error","msg":"meeting_service_unavailable","stage":"join",)"
            R"("reason":"getMeetingService returned nil; the SDK is not )"
            R"(authenticated or was not initialized."})");
        return;
    }

    if (!g_meeting_delegate) g_meeting_delegate = [[CVMeetingDelegate alloc] init];
    svc.delegate = g_meeting_delegate;

    long long meeting_number = 0;
    try {
        meeting_number = std::stoll(meeting_id);
    } catch (...) {
        EngineIpc::write(R"({"cmd":"error","msg":"invalid_meeting_id"})");
        return;
    }

    handle_stop_media("replacement_meeting");
    g_join_ctx = [[ZoomSDKJoinMeetingElements alloc] init];
    g_join_ctx.userType          = ZoomSDKUserType_WithoutLogin;
    g_join_ctx.meetingNumber     = meeting_number;
    g_join_ctx.displayName       = [NSString stringWithUTF8String:display_name.c_str()];
    g_join_ctx.password          = ns_or_nil(passcode);
    g_join_ctx.onBehalfToken     = ns_or_nil(on_behalf_token);
    g_join_ctx.zak               = ns_or_nil(user_zak);
    g_join_ctx.appPrivilegeToken = ns_or_nil(app_privilege_token);
    g_join_ctx.isNoVideo         = NO;
    g_join_ctx.isNoAudio         = NO;
    g_join_ctx.isMyVoiceInMix    = YES;
    // Match the Windows engine's raw-media negotiation exactly: the plugin's
    // read side assumes 48 kHz PCM and full-range BT.709 I420.
    g_join_ctx.audioRawdataSamplingRate = ZoomSDKAudioRawdataSamplingRate_48K;
    g_join_ctx.videoRawdataColorspace   = ZoomSDKVideoRawdataColorspace_BT709_F;
    // Stereo raw audio (default is mono). When the sender provides stereo
    // (Original Sound with stereo enabled) the SDK delivers two real
    // channels; mono senders arrive duplicated to both. The whole pipeline
    // reads the channel count from the frame header, and mono-preference
    // sources downmix in the plugin, so requesting stereo costs only
    // bandwidth on an all-mono meeting.
    g_join_ctx.isAudioRawDataStereo = YES;

    const ZoomSDKError err = [svc joinMeeting:g_join_ctx];
    EngineIpc::write(R"({"cmd":"debug","stage":"after_join","code":)" +
                     std::to_string(static_cast<int>(err)) + "}");
    if (err != ZoomSDKError_Success) {
        // A synchronous rejection means onMeetingStatusChange will never fire,
        // so without this the plugin waits on a status that is not coming and
        // eventually reports the engine as hung — blaming the transport for a
        // rejected argument.
        //
        // Deliberately NOT reported as "meeting_failed": that message carries a
        // MeetingFailCode, and this is a ZoomSDKError from the call itself. The
        // two enums overlap numerically (5 is InvalidParameter here but
        // MEETING_FAIL_SESSION_ERR there), so reusing it would hand the plugin's
        // error catalog a confidently wrong diagnosis.
        EngineIpc::write(R"({"cmd":"error","msg":"join_rejected","stage":"join","code":)" +
                         std::to_string(static_cast<int>(err)) +
                         R"(,"reason":"joinMeeting: was rejected by the Meeting SDK )"
                         R"(before contacting Zoom (ZoomSDKError )" +
                         std::to_string(static_cast<int>(err)) +
                         R"(). Check the meeting number and join tokens."})");
    }
}

static void handle_leave()
{
    handle_stop_media("meeting_leave_requested");
    ZoomSDKMeetingService *svc = meeting_service();
    if (svc) [svc leaveMeetingWithCmd:LeaveMeetingCmd_Leave];
}

// ── Raw media: shared helpers ────────────────────────────────────────────────
// Same validators as engine/src/main.cpp. A source uuid becomes part of a POSIX
// shm object name, so the character set is deliberately narrow.
static uint32_t json_uint(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    try {
        return static_cast<uint32_t>(std::stoul(json.substr(pos)));
    } catch (...) {
        return 0;
    }
}

static bool is_valid_source_uuid(const std::string &uuid)
{
    if (uuid.empty() || uuid.size() > 64) return false;
    return std::all_of(uuid.begin(), uuid.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

static ZoomSDKRawDataController *raw_data_controller()
{
    return [[ZoomSDK sharedSDK] getRawDataController];
}

// Seqlock write, byte-identical to the Windows engine's: bump the sequence to an
// odd value, publish the header, copy the payload, then bump it to even. The
// plugin's reader spins until it sees a stable even sequence, so an odd value
// means "a write is in flight, discard".
static uint32_t shm_seq_begin(uint32_t current)
{
    uint32_t seq = current + 1;
    if ((seq & 1u) == 0) ++seq;
    return seq;
}

// ── Raw video ────────────────────────────────────────────────────────────────
// One ZoomSDKRenderer per participant, fanned out to every source uuid bound to
// that participant — the same shape as ParticipantSubscription in
// engine/src/engine-video.cpp, and for the same reason: two OBS sources showing
// the same participant must not cost two Zoom subscriptions.
//
// The SDK delivers onRawDataReceived: on its own thread, so unlike the roster
// this state IS mutex-guarded. CVRenderer deliberately holds only a POD ivar and
// forwards into C++ — keeping the containers at file scope avoids non-trivial
// C++ ivars inside an ObjC class.
struct VideoTarget {
    ShmRegion shm;
    uint64_t  frame_count = 0;
    uint32_t  shm_gen = 0;          // bumped per region (re)create; sent per frame
    uint32_t  requested_resolution = 1; // desired quality survives recovery
    bool      shm_fail_reported = false;
};

struct VideoSubscription {
    uint32_t participant_id = 0;
    uint32_t resolution = 1;
    ZoomSDKRenderer *renderer = nil;
    id delegate = nil;              // CVRenderer, retained for the renderer's life
    std::unordered_map<std::string, VideoTarget> targets;
};

static std::mutex g_video_mtx;
static std::unordered_map<uint32_t, VideoSubscription> g_video;      // by participant
static std::unordered_map<std::string, uint32_t> g_source_participant;
static bool g_raw_media_active = false;

static void video_on_frame(uint32_t participant_id, ZoomSDKYUVRawDataI420 *data);

@interface CVRenderer : NSObject <ZoomSDKRendererDelegate>
{
    uint32_t _participantId;
}
- (instancetype)initWithParticipant:(uint32_t)participantId;
@end

@implementation CVRenderer

- (instancetype)initWithParticipant:(uint32_t)participantId
{
    if ((self = [super init])) _participantId = participantId;
    return self;
}

- (void)onRawDataReceived:(ZoomSDKYUVRawDataI420 *)data
{
    video_on_frame(_participantId, data);
}

- (void)onSubscribedUserDataOn
{
    EngineIpc::write(R"({"cmd":"debug","stage":"video_raw_status","participant_id":)" +
                     std::to_string(_participantId) + R"(,"status":1})");
}

- (void)onSubscribedUserDataOff
{
    EngineIpc::write(R"({"cmd":"debug","stage":"video_raw_status","participant_id":)" +
                     std::to_string(_participantId) + R"(,"status":0})");
}

- (void)onSubscribedUserLeft {}

- (void)onRendererBeDestroyed
{
    // The SDK is tearing the renderer down under us; drop our pointer so no
    // later unSubscribe/destroyRender touches freed memory.
    std::lock_guard<std::mutex> lock(g_video_mtx);
    auto it = g_video.find(_participantId);
    if (it != g_video.end()) it->second.renderer = nil;
}

@end

static ZoomSDKResolution sdk_resolution(uint32_t resolution)
{
    switch (resolution) {
    case 0:  return ZoomSDKResolution_360P;
    case 2:  return ZoomSDKResolution_1080P;
    case 1:
    default: return ZoomSDKResolution_720P;
    }
}

// Validate a frame and CROP it to even dimensions, reporting the cropped size
// through w/h.
//
// The SHM payload is laid out as y_len + y_len/4 + y_len/4, which only describes
// an I420 frame whose width and height are both even — for odd dimensions the
// real chroma planes are ceil(w/2) x ceil(h/2), which is larger than y_len/4.
// engine-video.cpp and engine-share.cpp handle that by REJECTING odd frames.
// That is fine for camera video, which is always even, and quietly fatal for
// screen share: a shared window is whatever size the window happens to be, and
// on macOS that is routinely odd (observed 1728x1117), so every single share
// frame was discarded as invalid and the source stayed black.
//
// Cropping one row/column is imperceptible and keeps the wire format and the
// plugin's reader exactly as they are, so that is what we do instead. The
// caller must keep the ORIGINAL width for stride arithmetic — see
// copy_i420_even.
static bool valid_i420_frame(ZoomSDKYUVRawDataI420 *data, uint32_t &w, uint32_t &h,
                             size_t &y_len)
{
    if (w == 0 || h == 0) return false;
    if (w > 8192 || h > 8192) return false;
    if (![data getYBuffer] || ![data getUBuffer] || ![data getVBuffer]) return false;

    w &= ~1u;
    h &= ~1u;
    if (w == 0 || h == 0) return false;

    y_len = static_cast<size_t>(w) * static_cast<size_t>(h);
    return true;
}

// Copy an I420 frame into the SHM payload, cropping to the even dimensions
// valid_i420_frame settled on. `src_w` is the frame's real width, which sets the
// source strides: the macOS SDK exposes no stride accessors on
// ZoomSDKYUVRawDataI420, so planes are assumed tightly packed at w and
// ceil(w/2). When nothing was cropped this degenerates to the same three plain
// memcpys the Windows engine does.
static void copy_i420_even(char *dst, ZoomSDKYUVRawDataI420 *data,
                           uint32_t w, uint32_t h, uint32_t src_w)
{
    const char *sy = [data getYBuffer];
    const char *su = [data getUBuffer];
    const char *sv = [data getVBuffer];
    const size_t y_len = static_cast<size_t>(w) * static_cast<size_t>(h);

    if (w == src_w) {
        std::memcpy(dst, sy, y_len);
    } else {
        for (uint32_t r = 0; r < h; ++r)
            std::memcpy(dst + static_cast<size_t>(r) * w,
                        sy + static_cast<size_t>(r) * src_w, w);
    }

    const uint32_t src_cstride = (src_w + 1) / 2;
    const uint32_t cw = w / 2;
    const uint32_t ch = h / 2;
    char *du = dst + y_len;
    char *dv = du + static_cast<size_t>(cw) * ch;
    if (cw == src_cstride) {
        std::memcpy(du, su, static_cast<size_t>(cw) * ch);
        std::memcpy(dv, sv, static_cast<size_t>(cw) * ch);
    } else {
        for (uint32_t r = 0; r < ch; ++r) {
            std::memcpy(du + static_cast<size_t>(r) * cw,
                        su + static_cast<size_t>(r) * src_cstride, cw);
            std::memcpy(dv + static_cast<size_t>(r) * cw,
                        sv + static_cast<size_t>(r) * src_cstride, cw);
        }
    }
}

static bool video_ensure_shm(VideoTarget &target, const std::string &source_uuid,
                             size_t y_len)
{
    const size_t total = sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (total < y_len) return false;
    if (target.shm.ptr && target.shm.size >= total) return true;

    // Generation-suffixed name from the shared per-base-name table: the
    // plugin derives the object name from the event's shm_gen via
    // shm_region_name(), so the writer must allocate through the same
    // machinery or reader and writer diverge on the name (see
    // src/shm-generation.h for why the counter cannot live in this struct).
    const ShmRegionAllocation region =
        shm_next_region(shm_generations(), IPC_SHM_PREFIX + source_uuid);
    if (!shm_region_create(target.shm, region.name, total)) return false;
    target.shm_gen = region.gen; // any plugin-side mapping is now stale
    if (target.shm.already_existed) {
        EngineIpc::write(
            R"({"cmd":"error","msg":"shm_name_collision","source_uuid":")" +
            source_uuid + "\"}");
    }
    return true;
}

static void video_on_frame(uint32_t participant_id, ZoomSDKYUVRawDataI420 *data)
{
    if (!data) return;
    const uint32_t src_w = [data getStreamWidth];
    const uint32_t src_h = [data getStreamHeight];
    uint32_t w = src_w, h = src_h;
    size_t y_len = 0;
    if (!valid_i420_frame(data, w, h, y_len)) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_frame_invalid","participant_id":)" +
            std::to_string(participant_id) + R"(,"w":)" + std::to_string(src_w) +
            R"(,"h":)" + std::to_string(src_h) + "}");
        return;
    }

    // try_lock, never block: this runs on an SDK raw-data thread, and the SDK
    // may wait for in-flight callbacks inside unSubscribe/destroyRender — which
    // the main thread calls while holding g_video_mtx. Blocking here would
    // recreate the renderer-teardown deadlock as an ABBA variant. Losing one
    // frame under contention is invisible at 25–30 fps.
    std::unique_lock<std::mutex> lock(g_video_mtx, std::try_to_lock);
    if (!lock.owns_lock()) return;
    auto sub = g_video.find(participant_id);
    if (sub == g_video.end()) return;

    for (auto &entry : sub->second.targets) {
        const std::string &source_uuid = entry.first;
        VideoTarget &target = entry.second;

        if (!video_ensure_shm(target, source_uuid, y_len) || !target.shm.ptr) {
            // Once per failure episode, not once per frame.
            if (!target.shm_fail_reported) {
                target.shm_fail_reported = true;
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(participant_id) + R"(,"w":)" + std::to_string(w) +
                    R"(,"h":)" + std::to_string(h) + "}");
                EngineIpc::write(
                    R"({"cmd":"error","msg":"shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(participant_id) + R"(,"w":)" + std::to_string(w) +
                    R"(,"h":)" + std::to_string(h) + "}");
            }
            continue;
        }
        if (target.shm_fail_reported) {
            target.shm_fail_reported = false;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_shm_recovered","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(participant_id) + "}");
        }

        auto *hdr    = static_cast<ShmFrameHeader *>(target.shm.ptr);
        auto *pixels = static_cast<char *>(target.shm.ptr) + sizeof(ShmFrameHeader);
        const uint32_t seq = shm_seq_begin(hdr->sequence);
        hdr->sequence = seq;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->width  = w;
        hdr->height = h;
        hdr->y_len  = static_cast<uint32_t>(y_len);

        copy_i420_even(pixels, data, w, h, src_w);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->sequence = seq + 1;

        ++target.frame_count;
        if (target.frame_count == 1 && (w != src_w || h != src_h)) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_frame_cropped_to_even","source_uuid":")" +
                source_uuid + R"(","src_w":)" + std::to_string(src_w) +
                R"(,"src_h":)" + std::to_string(src_h) + R"(,"w":)" +
                std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        }
        if (target.frame_count == 1 || target.frame_count % 120 == 0) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_frame_received","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(participant_id) + R"(,"count":)" +
                std::to_string(target.frame_count) + R"(,"w":)" + std::to_string(w) +
                R"(,"h":)" + std::to_string(h) + "}");
        }

        EngineIpc::write(
            R"({"cmd":"frame","source_uuid":")" + source_uuid +
            R"(","participant_id":)" + std::to_string(participant_id) +
            R"(,"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) +
            R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
    }
}

// Caller must hold g_video_mtx.
static void video_teardown_locked(uint32_t participant_id)
{
    auto it = g_video.find(participant_id);
    if (it == g_video.end()) return;
    if (it->second.renderer) {
        // Detach the delegate FIRST: destroyRender invokes
        // onRendererBeDestroyed synchronously on this thread, and that
        // callback locks g_video_mtx — which the caller already holds.
        // Verified live: this exact cycle deadlocked the main thread
        // (sample: video_teardown_locked → ZoomSDK → onRendererBeDestroyed
        // → mutex::lock), freezing the SDK UI and all video for hours.
        it->second.renderer.delegate = nil;
        [it->second.renderer unSubscribe];
        ZoomSDKRawDataController *rdc = raw_data_controller();
        if (rdc) [rdc destroyRender:it->second.renderer];
        it->second.renderer = nil;
    }
    [it->second.delegate release];
    it->second.delegate = nil;
    for (auto &t : it->second.targets) shm_region_destroy(t.second.shm);
    g_video.erase(it);
}

static void video_subscribe(uint32_t participant_id, const std::string &source_uuid,
                            uint32_t resolution)
{
    if (participant_id == 0) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_skipped","source_uuid":")" +
            source_uuid + R"(","participant_id":0,"reason":"missing_participant"})");
        return;
    }
    if (resolution > 2) resolution = 1;

    std::lock_guard<std::mutex> lock(g_video_mtx);

    // Rebinding a source to a different participant must release the old one.
    auto bound = g_source_participant.find(source_uuid);
    if (bound != g_source_participant.end() && bound->second != participant_id) {
        auto old = g_video.find(bound->second);
        if (old != g_video.end()) {
            auto t = old->second.targets.find(source_uuid);
            if (t != old->second.targets.end()) {
                shm_region_destroy(t->second.shm);
                old->second.targets.erase(t);
            }
            if (old->second.targets.empty()) video_teardown_locked(bound->second);
        }
    }
    g_source_participant[source_uuid] = participant_id;

    auto &pending = g_video[participant_id];
    pending.participant_id = participant_id;
    pending.targets[source_uuid].requested_resolution = resolution;
    if (pending.renderer) return; // one live renderer serves this participant
    pending.resolution = resolution;
    if (!g_raw_media_active) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_deferred","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + "}");
        return;
    }

    ZoomSDKRawDataController *rdc = raw_data_controller();
    if (!rdc) {
        EngineIpc::write(
            R"({"cmd":"error","msg":"raw_data_controller_unavailable","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + "}");
        return;
    }

    // Walk the requested resolution down on failure, exactly as the Windows
    // engine does — a 1080p subscription that the account or link will not carry
    // should degrade, not drop the participant entirely.
    for (int candidate = static_cast<int>(resolution); candidate >= 0; --candidate) {
        ZoomSDKRenderer *renderer = nil;
        const ZoomSDKError create_err = [rdc createRender:&renderer];
        if (create_err != ZoomSDKError_Success || !renderer) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"create_renderer_failed","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(participant_id) + R"(,"code":)" +
                std::to_string(static_cast<int>(create_err)) + R"(,"resolution":)" +
                std::to_string(candidate) + "}");
            continue;
        }

        CVRenderer *delegate = [[CVRenderer alloc] initWithParticipant:participant_id];
        renderer.delegate = delegate;

        const ZoomSDKError res_err =
            [renderer setResolution:sdk_resolution(static_cast<uint32_t>(candidate))];
        EngineIpc::write(
            R"({"cmd":"debug","stage":"set_resolution","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(res_err)) + R"(,"resolution":)" +
            std::to_string(candidate) + "}");

        const ZoomSDKError sub_err =
            [renderer subscribe:participant_id rawDataType:ZoomSDKRawDataType_Video];
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(sub_err)) + R"(,"resolution":)" +
            std::to_string(candidate) + "}");

        if (sub_err == ZoomSDKError_Success) {
            pending.resolution = static_cast<uint32_t>(candidate);
            pending.renderer = renderer;
            pending.delegate = delegate;
            if (static_cast<uint32_t>(candidate) != resolution) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_resolution_downgraded","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(participant_id) + R"(,"requested":)" +
                    std::to_string(resolution) + R"(,"actual":)" +
                    std::to_string(candidate) + "}");
            }
            return;
        }

        renderer.delegate = nil;
        [delegate release];
        [rdc destroyRender:renderer];
    }

    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_subscribe_failed_all","source_uuid":")" +
        source_uuid + R"(","participant_id":)" + std::to_string(participant_id) +
        R"(,"requested":)" + std::to_string(resolution) + "}");
    EngineIpc::write(
        R"({"cmd":"error","msg":"video_subscribe_failed","source_uuid":")" +
        source_uuid + R"(","participant_id":)" + std::to_string(participant_id) + "}");
}

static void video_unsubscribe(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(g_video_mtx);
    auto bound = g_source_participant.find(source_uuid);
    if (bound == g_source_participant.end()) return;
    const uint32_t participant_id = bound->second;
    g_source_participant.erase(bound);

    auto sub = g_video.find(participant_id);
    if (sub == g_video.end()) return;
    auto t = sub->second.targets.find(source_uuid);
    if (t != sub->second.targets.end()) {
        shm_region_destroy(t->second.shm);
        sub->second.targets.erase(t);
    }
    if (sub->second.targets.empty()) video_teardown_locked(participant_id);
}

// ── Raw screen share ─────────────────────────────────────────────────────────
// Share differs from participant video in one structural way: there is a single
// renderer for whatever share is currently viewable, not one per source. The
// share source id is chosen by the SDK and changes whenever someone starts,
// stops, or switches what they are sharing, so the renderer is torn down and
// rebuilt on those transitions while the SHM targets (one per OBS source bound
// in screenshare mode) persist across them.
struct ShareTarget {
    ShmRegion shm;
    uint64_t  frame_count = 0;
    uint32_t  shm_gen = 0;
    bool      shm_fail_reported = false;
};

static std::mutex g_share_mtx;
static std::unordered_map<std::string, ShareTarget> g_share_targets;
static ZoomSDKRenderer *g_share_renderer = nil;
static id g_share_renderer_delegate = nil;
static uint32_t g_share_source_id = 0;

static ZoomSDKASController *as_controller()
{
    ZoomSDKMeetingService *svc = meeting_service();
    return svc ? [svc getASController] : nil;
}

static void share_on_frame(ZoomSDKYUVRawDataI420 *data);

@interface CVShareRenderer : NSObject <ZoomSDKRendererDelegate>
@end

@implementation CVShareRenderer

- (void)onRawDataReceived:(ZoomSDKYUVRawDataI420 *)data { share_on_frame(data); }

- (void)onSubscribedUserDataOn
{
    EngineIpc::write(R"({"cmd":"debug","stage":"share_raw_status","status":1})");
}

- (void)onSubscribedUserDataOff
{
    EngineIpc::write(R"({"cmd":"debug","stage":"share_raw_status","status":0})");
}

- (void)onSubscribedUserLeft {}

- (void)onRendererBeDestroyed
{
    std::lock_guard<std::mutex> lock(g_share_mtx);
    g_share_renderer = nil;
    g_share_source_id = 0;
}

@end

static bool share_ensure_shm(ShareTarget &target, const std::string &source_uuid,
                             size_t y_len)
{
    const size_t total = sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (total < y_len) return false;
    if (target.shm.ptr && target.shm.size >= total) return true;

    const ShmRegionAllocation region =
        shm_next_region(shm_generations(), IPC_SHM_PREFIX + source_uuid);
    if (!shm_region_create(target.shm, region.name, total)) return false;
    target.shm_gen = region.gen;
    if (target.shm.already_existed) {
        EngineIpc::write(
            R"({"cmd":"error","msg":"shm_name_collision","source_uuid":")" +
            source_uuid + "\"}");
    }
    return true;
}

static void share_on_frame(ZoomSDKYUVRawDataI420 *data)
{
    if (!data) return;
    const uint32_t src_w = [data getStreamWidth];
    const uint32_t src_h = [data getStreamHeight];
    uint32_t w = src_w, h = src_h;
    size_t y_len = 0;
    if (!valid_i420_frame(data, w, h, y_len)) {
        EngineIpc::write(R"({"cmd":"debug","stage":"share_frame_invalid","w":)" +
                         std::to_string(src_w) + R"(,"h":)" +
                         std::to_string(src_h) + "}");
        return;
    }

    // The plugin keys share frames by the SHARING participant, matching the
    // Windows engine, so an operator sees the share attributed to a person.
    const uint32_t share_user_id = g_active_share_user.load(std::memory_order_acquire);

    // try_lock for the same reason as video_on_frame: never block an SDK
    // callback thread on a mutex the main thread holds across SDK teardown
    // calls. A dropped share frame at 1-30 fps is repainted by the next one.
    std::unique_lock<std::mutex> lock(g_share_mtx, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (auto &entry : g_share_targets) {
        const std::string &source_uuid = entry.first;
        ShareTarget &target = entry.second;

        if (!share_ensure_shm(target, source_uuid, y_len) || !target.shm.ptr) {
            if (!target.shm_fail_reported) {
                target.shm_fail_reported = true;
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"share_shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","w":)" + std::to_string(w) +
                    R"(,"h":)" + std::to_string(h) + "}");
                EngineIpc::write(
                    R"({"cmd":"error","msg":"shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","w":)" + std::to_string(w) +
                    R"(,"h":)" + std::to_string(h) + "}");
            }
            continue;
        }
        if (target.shm_fail_reported) {
            target.shm_fail_reported = false;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"share_shm_recovered","source_uuid":")" +
                source_uuid + "\"}");
        }

        auto *hdr    = static_cast<ShmFrameHeader *>(target.shm.ptr);
        auto *pixels = static_cast<char *>(target.shm.ptr) + sizeof(ShmFrameHeader);
        const uint32_t seq = shm_seq_begin(hdr->sequence);
        hdr->sequence = seq;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->width  = w;
        hdr->height = h;
        hdr->y_len  = static_cast<uint32_t>(y_len);

        copy_i420_even(pixels, data, w, h, src_w);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->sequence = seq + 1;

        ++target.frame_count;
        if (target.frame_count == 1 && (w != src_w || h != src_h)) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"share_frame_cropped_to_even","source_uuid":")" +
                source_uuid + R"(","src_w":)" + std::to_string(src_w) +
                R"(,"src_h":)" + std::to_string(src_h) + R"(,"w":)" +
                std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        }
        if (target.frame_count == 1 || target.frame_count % 120 == 0) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"share_frame_received","source_uuid":")" +
                source_uuid + R"(","share_source_id":)" +
                std::to_string(g_share_source_id) + R"(,"count":)" +
                std::to_string(target.frame_count) + R"(,"w":)" + std::to_string(w) +
                R"(,"h":)" + std::to_string(h) + "}");
        }

        EngineIpc::write(
            R"({"cmd":"frame","source_uuid":")" + source_uuid +
            R"(","participant_id":)" + std::to_string(share_user_id) +
            R"(,"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) +
            R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
    }
}

// Caller must hold g_share_mtx.
static void share_unsubscribe_renderer_locked()
{
    ZoomSDKRenderer *renderer = g_share_renderer;
    g_share_renderer = nil;
    g_share_source_id = 0;
    if (!renderer) return;
    // Same synchronous-callback deadlock as video_teardown_locked: detach the
    // delegate before destroyRender or onRendererBeDestroyed relocks
    // g_share_mtx on this thread.
    renderer.delegate = nil;
    [renderer unSubscribe];
    ZoomSDKRawDataController *rdc = raw_data_controller();
    if (rdc) [rdc destroyRender:renderer];
    [g_share_renderer_delegate release];
    g_share_renderer_delegate = nil;
}

// Caller must hold g_share_mtx.
static bool share_subscribe_to_locked(uint32_t share_source_id, const char *reason)
{
    if (share_source_id == 0) return false;
    if (g_share_renderer && g_share_source_id == share_source_id) return true;

    share_unsubscribe_renderer_locked();

    ZoomSDKRawDataController *rdc = raw_data_controller();
    if (!rdc) return false;
    ZoomSDKRenderer *renderer = nil;
    const ZoomSDKError create_err = [rdc createRender:&renderer];
    if (create_err != ZoomSDKError_Success || !renderer) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_create_renderer_failed","code":)" +
            std::to_string(static_cast<int>(create_err)) +
            R"(,"share_source_id":)" + std::to_string(share_source_id) + "}");
        return false;
    }

    CVShareRenderer *delegate = [[CVShareRenderer alloc] init];
    renderer.delegate = delegate;

    const ZoomSDKError res_err = [renderer setResolution:ZoomSDKResolution_1080P];
    EngineIpc::write(R"({"cmd":"debug","stage":"share_set_resolution","code":)" +
                     std::to_string(static_cast<int>(res_err)) +
                     R"(,"share_source_id":)" + std::to_string(share_source_id) + "}");

    const ZoomSDKError err =
        [renderer subscribe:share_source_id rawDataType:ZoomSDKRawDataType_Share];
    EngineIpc::write(R"({"cmd":"debug","stage":"share_subscribe","code":)" +
                     std::to_string(static_cast<int>(err)) +
                     R"(,"share_source_id":)" + std::to_string(share_source_id) +
                     R"(,"reason":")" + std::string(reason ? reason : "unknown") + "\"}");
    if (err != ZoomSDKError_Success) {
        renderer.delegate = nil;
        [delegate release];
        [rdc destroyRender:renderer];
        return false;
    }

    g_share_renderer = renderer;
    g_share_renderer_delegate = delegate;
    g_share_source_id = share_source_id;
    return true;
}

// Find whatever share is currently viewable and subscribe to it. Caller must
// hold g_share_mtx.
static void share_subscribe_active_locked(const char *reason)
{
    ZoomSDKASController *as = as_controller();
    if (!as) {
        EngineIpc::write(R"({"cmd":"debug","stage":"share_controller","code":-1})");
        return;
    }
    NSArray<NSNumber *> *sharers = [as getViewableSharingUserList];
    for (NSNumber *uid in sharers) {
        const unsigned int user_id = uid.unsignedIntValue;
        NSArray<ZoomSDKSharingSourceInfo *> *sources =
            [as getSharingSourceInfoList:user_id];
        for (ZoomSDKSharingSourceInfo *info in sources) {
            if (info.shareSourceID == 0) continue;
            g_active_share_user.store(user_id, std::memory_order_release);
            share_subscribe_to_locked(info.shareSourceID, reason);
            return;
        }
    }
    // Nobody is sharing. Drop any renderer we still hold (mirrors the Windows
    // engine's share_unavailable branch) — a subscription to a share that no
    // longer exists is exactly the state that crashed the SDK in live testing.
    share_unsubscribe_renderer_locked();
    g_active_share_user.store(0, std::memory_order_release);
    EngineIpc::write(R"({"cmd":"debug","stage":"share_none_active","reason":")" +
                     std::string(reason ? reason : "unknown") + R"(","sharers":)" +
                     std::to_string(sharers.count) + "}");
}

static void share_subscribe(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(g_share_mtx);
    const bool present = g_share_targets.find(source_uuid) != g_share_targets.end();
    if (shm_source_over_cap(g_share_targets.size(), present)) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_subscribe_rejected_capacity","source_uuid":")" +
            source_uuid + R"(","limit":)" + std::to_string(kMaxShmSources) + "}");
        EngineIpc::write(
            R"({"cmd":"error","msg":"subscribe_rejected","reason":"shm_capacity","source_uuid":")" +
            source_uuid + R"(","limit":)" + std::to_string(kMaxShmSources) + "}");
        return;
    }
    g_share_targets.emplace(source_uuid, ShareTarget{});
    if (!g_raw_media_active) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_subscribe_deferred","source_uuid":")" +
            source_uuid + "\"}");
        return;
    }
    share_subscribe_active_locked("share_subscribe");
}

static void share_unsubscribe(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(g_share_mtx);
    auto it = g_share_targets.find(source_uuid);
    if (it == g_share_targets.end()) return;
    shm_region_destroy(it->second.shm);
    g_share_targets.erase(it);
    // Last consumer gone: drop the Zoom subscription too rather than decoding a
    // share nobody is showing.
    if (g_share_targets.empty()) share_unsubscribe_renderer_locked();
}

static void share_teardown()
{
    std::lock_guard<std::mutex> lock(g_share_mtx);
    share_unsubscribe_renderer_locked();
    for (auto &entry : g_share_targets) shm_region_destroy(entry.second.shm);
    g_share_targets.clear();
    g_active_share_user.store(0, std::memory_order_release);
}

@interface CVShareDelegate : NSObject <ZoomSDKASControllerDelegate>
@end

// The SDK does not support raw-data subscriptions to the local user's own
// share: a self share never appears in getViewableSharingUserList, delivers at
// most one frame, and a renderer left bound to it after SelfEnd crashed the
// SDK in live testing (engine killed by signal ~16s later). The Windows engine
// is immune by construction — its Sharing_Self_* enum values fall through the
// event switch — but on macOS onShareContentChanged fires for self shares with
// the same signature as everyone else's, so filter by user id.
static bool share_is_self(unsigned int user_id)
{
    ZoomSDKMeetingActionController *ctrl = action_controller();
    ZoomSDKUserInfo *me = ctrl ? [ctrl getMyself] : nil;
    return me && user_id != 0 && [me getUserID] == user_id;
}

@implementation CVShareDelegate

- (void)onSharingStatusChanged:(ZoomSDKSharingSourceInfo *)shareInfo
{
    if (!shareInfo) return;
    const bool self_share = share_is_self(shareInfo.userID);
    EngineIpc::write(R"({"cmd":"debug","stage":"share_status","user_id":)" +
                     std::to_string(shareInfo.userID) +
                     R"(,"share_source_id":)" + std::to_string(shareInfo.shareSourceID) +
                     R"(,"status":)" +
                     std::to_string(static_cast<int>(shareInfo.status)) +
                     R"(,"self":)" + (self_share ? "true" : "false") + "}");

    // Scoped: rebuild_roster below calls back into the SDK (getUserList), and
    // SDK calls must not run under g_share_mtx — the SDK's own callbacks take
    // that mutex from its threads.
    {
    std::lock_guard<std::mutex> lock(g_share_mtx);
    switch (shareInfo.status) {
    case ZoomSDKShareStatus_OtherBegin:
    case ZoomSDKShareStatus_ViewOther:
    case ZoomSDKShareStatus_Resume:
        if (self_share) break;
        if (shareInfo.userID != 0)
            g_active_share_user.store(shareInfo.userID, std::memory_order_release);
        if (shareInfo.shareSourceID != 0)
            share_subscribe_to_locked(shareInfo.shareSourceID, "share_status");
        else
            share_subscribe_active_locked("share_status");
        break;
    case ZoomSDKShareStatus_SelfEnd:
        // Safety net only: we never subscribe to a self share, so tear down
        // solely if a renderer is somehow bound to this exact source.
        if (shareInfo.shareSourceID != 0 &&
            shareInfo.shareSourceID == g_share_source_id) {
            share_unsubscribe_renderer_locked();
            share_subscribe_active_locked("self_share_end");
        }
        break;
    case ZoomSDKShareStatus_OtherEnd:
        if (shareInfo.shareSourceID == 0 ||
            shareInfo.shareSourceID == g_share_source_id) {
            share_unsubscribe_renderer_locked();
            g_active_share_user.store(0, std::memory_order_release);
            // Someone else may still be sharing — re-evaluate rather than
            // assuming the meeting has no share left.
            share_subscribe_active_locked("share_end");
        }
        break;
    default:
        break;
    }
    } // release g_share_mtx before touching the SDK again
    rebuild_roster();
    send_roster();
}

- (void)onShareContentChanged:(ZoomSDKSharingSourceInfo *)shareInfo
{
    if (!shareInfo) return;
    const bool self_share = share_is_self(shareInfo.userID);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"share_content_changed","user_id":)" +
        std::to_string(shareInfo.userID) + R"(,"share_source_id":)" +
        std::to_string(shareInfo.shareSourceID) +
        R"(,"self":)" + (self_share ? "true" : "false") + "}");
    if (self_share) return; // see share_is_self
    std::lock_guard<std::mutex> lock(g_share_mtx);
    if (shareInfo.userID != 0)
        g_active_share_user.store(shareInfo.userID, std::memory_order_release);
    if (shareInfo.shareSourceID != 0)
        share_subscribe_to_locked(shareInfo.shareSourceID, "share_content_changed");
}

- (void)onFailedToStartShare
{
    EngineIpc::write(R"({"cmd":"debug","stage":"share_failed_to_start"})");
}

@end

static CVShareDelegate *g_share_delegate = nil;

static void share_attach()
{
    ZoomSDKASController *as = as_controller();
    if (!as) {
        EngineIpc::write(R"({"cmd":"debug","stage":"share_controller","code":-1})");
        return;
    }
    if (!g_share_delegate) g_share_delegate = [[CVShareDelegate alloc] init];
    as.delegate = g_share_delegate;
    std::lock_guard<std::mutex> lock(g_share_mtx);
    if (g_raw_media_active) share_subscribe_active_locked("meeting_joined");
}

// ── Raw audio ────────────────────────────────────────────────────────────────
// One subscription to the shared audio helper, fanned out per source. Routing
// matches engine/src/engine-audio.cpp: isolate → that participant's one-way
// audio only; audience → every non-isolated participant's one-way audio;
// neither → the full meeting mix.
struct AudioTarget {
    uint32_t participant_id = 0;
    bool     isolate_audio = false;
    bool     audience_audio = false;
    ShmRegion shm;
    uint64_t frame_count = 0;
    bool     shm_fail_reported = false;
    uint32_t shm_gen = 0;
    uint32_t notify_suppressed = 0;
    bool     slot_too_small_reported = false;
};

static std::mutex g_audio_mtx;
static std::unordered_map<std::string, AudioTarget> g_audio;
static bool g_audio_subscribed = false;

static bool audio_ensure_shm(AudioTarget &target, const std::string &source_uuid,
                             uint32_t byte_len)
{
    // Ring region, mirroring EngineAudio::ensure_shm() in engine-audio.cpp:
    // the plugin reader drains a slot ring and expects the header initialised
    // before the first publish. Same generation-suffixed naming as video.
    const size_t total = shm_audio_region_bytes(byte_len);
    if (target.shm.ptr && target.shm.size >= total) return true;

    const ShmRegionAllocation region =
        shm_next_region(shm_generations(),
                        IPC_SHM_PREFIX + source_uuid + "_audio");
    if (!shm_region_create(target.shm, region.name, total)) return false;
    target.shm_gen = region.gen;

    if (target.shm.already_existed) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_shm_name_collision","source_uuid":")" +
            source_uuid + R"(","shm_gen":)" + std::to_string(region.gen) + "}");
        EngineIpc::write(
            R"({"cmd":"error","msg":"shm_name_collision","source_uuid":")" +
            source_uuid + "\"}");
    }

    auto *ring = static_cast<ShmAudioHeader *>(target.shm.ptr);
    ring->write_index = 0;
    ring->slot_count  = kAudioRingSlots;
    ring->slot_bytes  = byte_len;
    ring->sample_rate = 0;
    ring->channels    = 0;
    ring->notify      = 0;   // clear: the first buffer always sends its event
    return true;
}

// Caller must hold g_audio_mtx.
static void audio_output_frame(AudioTarget &target, const std::string &source_uuid,
                               ZoomSDKAudioRawData *data, const char *stage)
{
    const uint32_t byte_len = [data getBufferLen];
    if (byte_len == 0) return;

    if (!audio_ensure_shm(target, source_uuid, byte_len) || !target.shm.ptr) {
        if (!target.shm_fail_reported) {
            target.shm_fail_reported = true;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"audio_shm_create_failed","source_uuid":")" +
                source_uuid + R"(","byte_len":)" + std::to_string(byte_len) + "}");
            EngineIpc::write(
                R"({"cmd":"error","msg":"shm_create_failed","source_uuid":")" +
                source_uuid + R"(","byte_len":)" + std::to_string(byte_len) + "}");
        }
        return;
    }
    if (target.shm_fail_reported) {
        target.shm_fail_reported = false;
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_shm_recovered","source_uuid":")" +
            source_uuid + "\"}");
    }

    auto *ring = static_cast<ShmAudioHeader *>(target.shm.ptr);
    // Unreachable given audio_ensure_shm()'s invariant; latched debug only
    // (see the proof in engine-audio.cpp -- an impossible check must not vote
    // against the meeting from a 100 Hz path).
    if (byte_len > ring->slot_bytes) {
        if (!target.slot_too_small_reported) {
            target.slot_too_small_reported = true;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"audio_slot_too_small","source_uuid":")" +
                source_uuid + R"(","byte_len":)" + std::to_string(byte_len) +
                R"(,"slot_bytes":)" + std::to_string(ring->slot_bytes) + "}");
        }
        return;
    }

    const uint32_t index = ring->write_index % ring->slot_count;
    auto *slot = reinterpret_cast<ShmAudioSlot *>(
        static_cast<char *>(target.shm.ptr) +
        shm_audio_slot_offset(*ring, index));

    uint32_t seq = slot->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    slot->sequence = seq;                       // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    slot->capture_ns     = tile_clock_now_ns();
    slot->byte_len       = byte_len;
    slot->participant_id = target.participant_id;
    std::memcpy(reinterpret_cast<char *>(slot) + sizeof(ShmAudioSlot),
                [data getBuffer], byte_len);
    std::atomic_thread_fence(std::memory_order_release);
    slot->sequence = seq + 1;                   // even: readable

    ring->sample_rate = [data getSampleRate];
    ring->channels    = static_cast<uint16_t>([data getChannelNum]);
    std::atomic_thread_fence(std::memory_order_release);
    // Published last; FREE-RUNNING, never % slot_count (see engine-ipc.h).
    ring->write_index = ring->write_index + 1;

    ++target.frame_count;
    if (target.frame_count == 1 || target.frame_count % 250 == 0) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":")" + std::string(stage) +
            R"(","source_uuid":")" + source_uuid + R"(","count":)" +
            std::to_string(target.frame_count) + R"(,"sample_rate":)" +
            std::to_string([data getSampleRate]) + R"(,"channels":)" +
            std::to_string([data getChannelNum]) + R"(,"byte_len":)" +
            std::to_string(byte_len) + R"(,"participant_id":)" +
            std::to_string(target.participant_id) + "}");
    }

    // Edge-triggered wakeup + 2.5s keepalive, mirroring engine-audio.cpp
    // (protocol and lost-wakeup proof: ShmAudioHeader::notify in engine-ipc.h).
    if (audio_ring_notify_after_publish(ring)) {
        target.notify_suppressed = 0;
        EngineIpc::write(
            R"({"cmd":"audio","source_uuid":")" + source_uuid +
            R"(","participant_id":)" + std::to_string(target.participant_id) +
            R"(,"byte_len":)" + std::to_string(byte_len) +
            R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
    } else if (++target.notify_suppressed >= 250) {
        target.notify_suppressed = 0;
        EngineIpc::write(
            R"({"cmd":"audio","source_uuid":")" + source_uuid +
            R"(","participant_id":)" + std::to_string(target.participant_id) +
            R"(,"byte_len":)" + std::to_string(byte_len) +
            R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
    }
}

@interface CVAudioDelegate : NSObject <ZoomSDKAudioRawDataDelegate>
@end

@implementation CVAudioDelegate

- (void)onMixedAudioRawDataReceived:(ZoomSDKAudioRawData *)data
{
    if (!data || [data getBufferLen] == 0) return;
    std::lock_guard<std::mutex> lock(g_audio_mtx);
    for (auto &entry : g_audio) {
        // isolate and audience targets both take one-way audio only.
        if (entry.second.isolate_audio || entry.second.audience_audio) continue;
        audio_output_frame(entry.second, entry.first, data, "audio_frame_received");
    }
}

- (void)onOneWayAudioRawDataReceived:(ZoomSDKAudioRawData *)data userID:(unsigned int)userID
{
    if (!data || [data getBufferLen] == 0) return;
    std::lock_guard<std::mutex> lock(g_audio_mtx);

    bool claimed_by_isolate = false;
    for (auto &entry : g_audio) {
        if (!entry.second.isolate_audio) continue;
        if (entry.second.participant_id != userID) continue;
        claimed_by_isolate = true;
        audio_output_frame(entry.second, entry.first, data,
                           "audio_one_way_frame_received");
    }
    if (claimed_by_isolate) return;

    for (auto &entry : g_audio) {
        if (!entry.second.audience_audio) continue;
        audio_output_frame(entry.second, entry.first, data,
                           "audio_audience_frame_received");
    }
}

- (void)onOneWayAudioRawDataReceived:(ZoomSDKAudioRawData *)data nodeID:(unsigned int)nodeID {}
- (void)onShareAudioRawDataReceived:(ZoomSDKAudioRawData *)data {}
- (void)onShareAudioRawDataReceived:(ZoomSDKAudioRawData *)data userID:(unsigned int)userID {}
- (void)onOneWayInterpreterAudioRawDataReceived:(ZoomSDKAudioRawData *)data
                                strLanguageName:(NSString *)languageName {}

@end

static CVAudioDelegate *g_audio_delegate = nil;

static bool audio_subscribe_if_needed(const std::string &source_uuid,
                                      const char *stage)
{
    if (g_audio_subscribed) return true;
    if (!g_raw_media_active) return false;

    ZoomSDKRawDataController *rdc = raw_data_controller();
    if (!rdc) return false;
    ZoomSDKAudioRawDataHelper *helper = nil;
    const ZoomSDKError get_err = [rdc getAudioRawDataHelper:&helper];
    if (get_err != ZoomSDKError_Success || !helper) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_helper_unavailable","source_uuid":")" +
            source_uuid + R"(","code":)" +
            std::to_string(static_cast<int>(get_err)) + "}");
        return false;
    }

    if (!g_audio_delegate) g_audio_delegate = [[CVAudioDelegate alloc] init];
    helper.delegate = g_audio_delegate;
    const ZoomSDKError err = [helper subscribe];
    EngineIpc::write(
        R"({"cmd":"debug","stage":")" + std::string(stage) +
        R"(","source_uuid":")" + source_uuid + R"(","code":)" +
        std::to_string(static_cast<int>(err)) + "}");
    if (err != ZoomSDKError_Success) return false;

    g_audio_subscribed = true;
    return true;
}

static void audio_init(const std::string &source_uuid, uint32_t participant_id,
                       bool isolate_audio, bool audience_audio)
{
    {
        std::lock_guard<std::mutex> lock(g_audio_mtx);
        AudioTarget &target = g_audio[source_uuid];
        target.participant_id = participant_id;
        // Both set is treated as isolate, matching engine-audio.cpp.
        target.isolate_audio = isolate_audio;
        target.audience_audio = isolate_audio ? false : audience_audio;
    }
    audio_subscribe_if_needed(source_uuid, "audio_subscribe");
}

static void audio_remove(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(g_audio_mtx);
    auto it = g_audio.find(source_uuid);
    if (it == g_audio.end()) return;
    shm_region_destroy(it->second.shm);
    g_audio.erase(it);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"audio_target_removed","source_uuid":")" +
        source_uuid + "\"}");
}

// ── start / stop raw media ───────────────────────────────────────────────────
// Raw data requires raw recording to be running. Subscriptions requested before
// that are held as pending bindings and created here, so the plugin can assign
// sources in any order relative to start_media.
static void resubscribe_raw_media(const char *reason)
{
    std::vector<std::tuple<uint32_t, std::string, uint32_t>> pending;
    {
        std::lock_guard<std::mutex> lock(g_video_mtx);
        for (auto &entry : g_video) {
            if (entry.second.renderer) continue;   // already live
            // Participant ids are room-scoped. Keep missing assignments pending
            // until the plugin resolves/rebinds them from the new roster.
            if (std::none_of(g_roster.begin(), g_roster.end(), [&](const auto &p) {
                    return p.user_id == entry.first;
                })) continue;
            // One attempt per participant; placeholders retain ALL bindings even
            // if the SDK rejects every resolution. Unsubscribe can still remove them.
            if (!entry.second.targets.empty()) {
                const auto &t = *entry.second.targets.begin();
                pending.emplace_back(entry.first, t.first, t.second.requested_resolution);
            }
        }
    }

    EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_resubscribe","reason":")" +
                     std::string(reason ? reason : "") + R"(","count":)" +
                     std::to_string(pending.size()) + "}");

    for (auto &p : pending)
        video_subscribe(std::get<0>(p), std::get<1>(p), std::get<2>(p));

    std::vector<std::string> audio_sources;
    {
        std::lock_guard<std::mutex> lock(g_audio_mtx);
        for (auto &entry : g_audio) audio_sources.push_back(entry.first);
    }
    if (!audio_sources.empty())
        audio_subscribe_if_needed(audio_sources.front(), "audio_resubscribe");

    // Share targets bound before raw media started are deferred the same way
    // video subscriptions are; pick up whatever is being shared now.
    {
        std::lock_guard<std::mutex> lock(g_share_mtx);
        if (!g_share_targets.empty())
            share_subscribe_active_locked(reason);
    }
}

static RawMediaLifecycle g_raw_lifecycle;
@class CVRecordDelegate;
static CVRecordDelegate *g_record_delegate = nil;

@interface CVRecordDelegate : NSObject <ZoomSDKMeetingRecordDelegate>
@end

@implementation CVRecordDelegate

- (void)onLocalRecordingPrivilegeRequestStatus:(ZoomSDKRequestLocalRecordingStatus)status
{
    EngineIpc::write(
        R"({"cmd":"debug","stage":"local_recording_privilege_status","status":)" +
        std::to_string(static_cast<int>(status)) + "}");
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_record_delegate != self) return;
        if (status == ZoomSDKRequestLocalRecordingStatus_Granted) {
            raw_media_event(RawMediaEvent::Grant, "privilege_granted");
        } else if (status == ZoomSDKRequestLocalRecordingStatus_Denied) {
            raw_media_event(RawMediaEvent::Denied, "privilege_denied");
        } else if (status == ZoomSDKRequestLocalRecordingStatus_Timeout) {
            raw_media_event(RawMediaEvent::Timeout, "privilege_request_timeout");
        }
    });
}

- (void)onRecordPrivilegeChange:(BOOL)canRec
{
    EngineIpc::write(R"({"cmd":"debug","stage":"record_privilege_change","can_record":)" +
                     std::string(canRec ? "true" : "false") + "}");
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_record_delegate != self) return;
        raw_media_event(canRec ? RawMediaEvent::Grant : RawMediaEvent::Denied,
                        canRec ? "privilege_changed" : "privilege_revoked");
    });
}

- (void)onRecord2MP4Done:(BOOL)success Path:(NSString *)recordPath {}
- (void)onRecord2MP4Progressing:(int)percentage {}
- (void)onCloudRecordingStatus:(ZoomSDKRecordingStatus)status {}
- (void)onCustomizedRecordingSourceReceived:(CustomizedRecordingLayoutHelper *)helper {}
- (void)onLocalRecordStatus:(ZoomSDKRecordingStatus)status userID:(unsigned int)userID {}
- (void)onLocalRecordingPrivilegeRequested:(ZoomSDKRequestLocalRecordingPrivilegeHandler *)handler {}
- (void)onCloudRecordingStorageFull:(time_t)gracePeriodDate {}
- (void)onRequestCloudRecordingResponse:(ZoomSDKRequestStartCloudRecordingStatus)status {}
- (void)onStartCloudRecordingRequested:(ZoomSDKRequestStartCloudRecordingHandler *)handler {}
- (void)onEnableAndStartSmartRecordingRequested:(ZoomSDKRequestEnableAndStartSmartRecordingHandler *)handler {}
- (void)onSmartRecordingEnableActionCallback:(ZoomSDKSmartRecordingEnableActionHandler *)handler {}

@end

// Invalidate SDK resources at a demonstrated lifecycle boundary, preserving
// desired bindings and SHM generations. Never use the destructive manual Stop
// path here: it erases assignments required for grant/room recovery.
static void suspend_raw_media()
{
    g_raw_media_active = false;
    {
        std::lock_guard<std::mutex> lock(g_video_mtx);
        for (auto &entry : g_video) {
            auto &sub = entry.second;
            if (sub.renderer) {
                sub.renderer.delegate = nil;
                [sub.renderer unSubscribe];
                auto *rdc = raw_data_controller();
                if (rdc) [rdc destroyRender:sub.renderer];
                sub.renderer = nil;
            }
            [sub.delegate release];
            sub.delegate = nil;
            // Restart the diagnostic first-frame counter, not SHM generation.
            for (auto &target : sub.targets) target.second.frame_count = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_share_mtx);
        share_unsubscribe_renderer_locked();
    }
    if (g_audio_subscribed) {
        ZoomSDKRawDataController *rdc = raw_data_controller();
        ZoomSDKAudioRawDataHelper *helper = nil;
        if (rdc && [rdc getAudioRawDataHelper:&helper] == ZoomSDKError_Success && helper)
            [helper unSubscribe];
        g_audio_subscribed = false;
    }
    auto *svc = meeting_service();
    auto *rec = svc ? [svc getRecordController] : nil;
    if (rec) {
        const auto err = [rec stopRawRecording];
        EngineIpc::write(R"({"cmd":"debug","stage":"stop_raw_recording","code":)" +
                         std::to_string(static_cast<int>(err)) + "}");
    }
}

static void raw_media_event(RawMediaEvent event, const char *reason, int sdk_code)
{
    // All callers either run on the command main queue or dispatch callbacks
    // onto it. The decision is committed before any potentially reentrant SDK call.
    const std::string before = g_raw_lifecycle.state();
    const RawMediaAction action = g_raw_lifecycle.on(event);
    if (before != g_raw_lifecycle.state() || event == RawMediaEvent::Timeout) {
        EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_state","state":")" +
                         std::string(g_raw_lifecycle.state()) + R"(","reason":")" +
                         std::string(reason ? reason : "") + "\"}");
    }
    if (action == RawMediaAction::None) return;
    if (action == RawMediaAction::Suspend) {
        suspend_raw_media();
        return;
    }
    if (action == RawMediaAction::Fail) {
        EngineIpc::write(R"({"cmd":"error","msg":"raw_media_start_failed","reason":")" +
                         std::string(reason ? reason : "") + R"(","code":)" +
                         std::to_string(sdk_code) +
                         R"(,"detail":"Raw media could not start. Check recording permission and SDK licensing, then retry Start. The meeting remains joined."})");
        return;
    }
    if (action == RawMediaAction::Restore) {
        g_raw_media_active = true;
        // Session readiness is NOT proof of healthy video; first frames and
        // individual subscribe failures remain the source-health authority.
        EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_ready","reason":")" +
                         std::string(reason ? reason : "") + "\"}");
        resubscribe_raw_media(reason);
        return;
    }
    auto *svc = meeting_service();
    auto *rec = svc ? [svc getRecordController] : nil;
    if (!rec) {
        raw_media_event(RawMediaEvent::CheckFailed, "recording_controller_unavailable", -1);
        return;
    }
    if (!g_record_delegate) g_record_delegate = [[CVRecordDelegate alloc] init];
    rec.delegate = g_record_delegate;
    if (action == RawMediaAction::Check) {
        const auto err = [rec canStartRawRecording];
        EngineIpc::write(R"({"cmd":"debug","stage":"can_start_raw_recording","code":)" +
                         std::to_string(static_cast<int>(err)) + "}");
        // Only NoPermission authorizes a host request. NoLicense (21), WrongUsage
        // and service errors are not evidence that the host withheld permission.
        raw_media_event(err == ZoomSDKError_Success ? RawMediaEvent::CheckReady :
                        err == ZoomSDKError_NoPermission ? RawMediaEvent::NoPermission :
                        RawMediaEvent::CheckFailed, "cannot_start_raw_recording", static_cast<int>(err));
    } else if (action == RawMediaAction::Request) {
        const auto support = [rec isSupportRequestLocalRecordingPrivilege];
        const auto err = support == ZoomSDKError_Success ? [rec requestLocalRecordingPrivilege] : support;
        EngineIpc::write(R"({"cmd":"debug","stage":"request_recording_privilege","code":)" +
                         std::to_string(static_cast<int>(err)) + "}");
        if (err != ZoomSDKError_Success) {
            raw_media_event(RawMediaEvent::RequestFailed, "recording_privilege_request_failed", static_cast<int>(err));
        } else {
            EngineIpc::write(R"({"cmd":"error","msg":"raw_media_start_failed","reason":"cannot_start_raw_recording","privilege_requested":true,"detail":"Raw recording needs local-recording permission. The meeting host must allow this participant to record."})");
        }
    } else if (action == RawMediaAction::Start) {
        const auto err = [rec startRawRecording];
        EngineIpc::write(R"({"cmd":"debug","stage":"start_raw_recording","code":)" +
                         std::to_string(static_cast<int>(err)) + "}");
        raw_media_event(err == ZoomSDKError_Success ? RawMediaEvent::StartSucceeded :
                        RawMediaEvent::StartFailed, err == ZoomSDKError_Success ? "raw_recording_started" :
                        "start_raw_recording_failed", static_cast<int>(err));
    }
}

static void handle_start_media(const char *reason)
{
    raw_media_event(RawMediaEvent::Start, reason);
}

static void handle_stop_media(const char *reason)
{
    g_raw_media_active = false;
    raw_media_event(RawMediaEvent::Stop, reason);
    if (std::string(reason ? reason : "") != "manual_stop")
        raw_media_event(RawMediaEvent::Reset, reason);
    auto *record_service = meeting_service();
    auto *record_controller = record_service ? [record_service getRecordController] : nil;
    if (record_controller && record_controller.delegate == g_record_delegate)
        record_controller.delegate = nil;
    [g_record_delegate release];
    g_record_delegate = nil; // queued callbacks from the retired delegate are ignored
    {
        std::lock_guard<std::mutex> lock(g_video_mtx);
        std::vector<uint32_t> ids;
        ids.reserve(g_video.size());
        for (auto &entry : g_video) ids.push_back(entry.first);
        for (uint32_t id : ids) video_teardown_locked(id);
        g_source_participant.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_audio_mtx);
        for (auto &entry : g_audio) shm_region_destroy(entry.second.shm);
        g_audio.clear();
    }
    share_teardown();
    if (g_audio_subscribed) {
        ZoomSDKRawDataController *rdc = raw_data_controller();
        ZoomSDKAudioRawDataHelper *helper = nil;
        if (rdc && [rdc getAudioRawDataHelper:&helper] == ZoomSDKError_Success && helper)
            [helper unSubscribe];
        g_audio_subscribed = false;
    }
    ZoomSDKMeetingService *svc = meeting_service();
    ZoomSDKMeetingRecordController *rec = svc ? [svc getRecordController] : nil;
    if (rec) {
        const ZoomSDKError err = [rec stopRawRecording];
        EngineIpc::write(R"({"cmd":"debug","stage":"stop_raw_recording","code":)" +
                         std::to_string(static_cast<int>(err)) + "}");
    }
    EngineIpc::write(R"({"cmd":"debug","stage":"raw_media_stopped","reason":")" +
                     std::string(reason ? reason : "") + "\"}");
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

    // Heartbeat, matching engine/src/main.cpp: ping every ~2s so the plugin can
    // tell a hung-but-alive engine from a quiet one. Its absence here was not
    // cosmetic — zoom-engine-client.cpp declares the engine dead after 10s of
    // IPC silence, so the macOS engine was reported as "stopped responding"
    // after every idle stretch, including a perfectly healthy sit in a meeting.
    // The ping is written from the MAIN QUEUE, not from this thread: every SDK
    // delegate and every subscribe/teardown runs on the main thread, so a ping
    // from a side thread vouches for an engine that may no longer be able to do
    // any work. A renderer-teardown deadlock froze the main thread for hours
    // while a detached-thread heartbeat kept the plugin convinced all was well.
    // Routed through the main queue, a hung main thread silences the heartbeat
    // and the plugin's 10 s watchdog kills and recovers the engine.
    std::thread heartbeat([]() {
        while (g_running.load(std::memory_order_acquire)) {
            for (int i = 0; i < 20 && g_running.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_running.load(std::memory_order_acquire)) break;
            if (g_heartbeat_via_main_queue.load(std::memory_order_acquire)) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (g_running.load(std::memory_order_acquire))
                        EngineIpc::write(R"({"cmd":"ping"})");
                });
            } else if (!EngineIpc::write(R"({"cmd":"ping"})")) {
                break;
            }
        }
    });
    heartbeat.detach();

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
            // Ordered like main.cpp's command loop. These are substring matches
            // on the whole line, so the order is the disambiguation: check the
            // longer/more specific commands before their prefixes.
            if (msg.find(IPC_CMD_INIT) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{ handle_init(msg); });
            } else if (msg.find(IPC_CMD_JOIN) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{ handle_join(msg); });
            } else if (msg.find(IPC_CMD_LEAVE) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{ handle_leave(); });
            } else if (msg.find(IPC_CMD_START_MEDIA) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(),
                               ^{ handle_start_media("manual_start"); });
            } else if (msg.find(IPC_CMD_STOP_MEDIA) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(),
                               ^{ handle_stop_media("manual_stop"); });
            } else if (msg.find(IPC_CMD_SUBSCRIBE_AUDIO) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    const std::string uuid = json_str(msg, "source_uuid");
                    if (!is_valid_source_uuid(uuid)) return;
                    audio_init(uuid, json_uint(msg, "participant_id"),
                               msg.find(R"("isolate_audio":true)") != std::string::npos,
                               msg.find(R"("audience_audio":true)") != std::string::npos);
                });
            // Checked BEFORE subscribe: "unsubscribe" contains "subscribe", so the
            // reverse order routes an unsubscribe into the subscribe branch. (The
            // Windows engine has that ordering and gets away with it only because
            // participant_id parses as 0 there, which its subscribe path treats as
            // an unsubscribe — but it never drops the audio target.)
            } else if (msg.find(IPC_CMD_UNSUBSCRIBE) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    const std::string uuid = json_str(msg, "source_uuid");
                    if (!is_valid_source_uuid(uuid)) return;
                    // The plugin does not say which kind the source was, so
                    // clear it from all three paths; each is a no-op if the
                    // source was never bound there.
                    video_unsubscribe(uuid);
                    share_unsubscribe(uuid);
                    audio_remove(uuid);
                });
            } else if (msg.find(IPC_CMD_SUBSCRIBE) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    const std::string uuid = json_str(msg, "source_uuid");
                    if (!is_valid_source_uuid(uuid)) return;
                    if (json_str(msg, "mode") == "screenshare") {
                        share_subscribe(uuid);
                        return;
                    }
                    uint32_t res = json_uint(msg, "resolution");
                    if (res > 2) res = 1;
                    const uint32_t pid = json_uint(msg, "participant_id");
                    video_subscribe(pid, uuid, res);
                    audio_init(uuid, pid,
                               msg.find(R"("isolate_audio":true)") != std::string::npos,
                               msg.find(R"("audience_audio":true)") != std::string::npos);
                });
            }
        }

        // Peer closed the socket or sent quit: unwind from the main thread,
        // which owns the run loop.
        dispatch_async(dispatch_get_main_queue(), ^{
            // Stop the heartbeat before closing the sockets, or it can write
            // into a descriptor that teardown has already closed (and that the
            // process may have handed to something else).
            g_running.store(false, std::memory_order_release);
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
    // NSApplication instance must exist.
    //
    // Regular activation policy, not Accessory: the SDK's meeting UI is enabled
    // (matching Windows), and an Accessory app has no menu bar and cannot make
    // its windows properly key — the meeting window would appear but behave
    // badly. The cost is a Dock icon for the engine while it runs.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp run];
    return 0;
}
