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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Set false during teardown so the heartbeat thread stops before the IPC
// sockets close. File scope because the heartbeat thread is detached and
// outlives any narrower scope.
static std::atomic<bool> g_running{true};

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

    // Screen share is not ported yet. Reporting false is the honest answer for
    // an unimplemented path; it is NOT a guess that nobody is sharing. When the
    // share controller lands, set this from the active share user id the way
    // main.cpp does.
    info.is_sharing_screen = false;
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
        rebuild_roster();
        send_roster();
        break;
    }
    case ZoomSDKMeetingStatus_Disconnecting:
    case ZoomSDKMeetingStatus_Ended: {
        ZoomSDKMeetingActionController *ctrl = action_controller();
        if (ctrl && ctrl.delegate == self) ctrl.delegate = nil;
        g_roster.clear();
        g_active_speaker = 0;
        EngineIpc::write(R"({"cmd":"left"})");
        break;
    }
    case ZoomSDKMeetingStatus_Failed: {
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
    ZoomSDKMeetingService *svc = meeting_service();
    if (svc) [svc leaveMeetingWithCmd:LeaveMeetingCmd_Leave];
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
    std::thread heartbeat([]() {
        while (g_running.load(std::memory_order_acquire)) {
            for (int i = 0; i < 20 && g_running.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_running.load(std::memory_order_acquire)) break;
            if (!EngineIpc::write(R"({"cmd":"ping"})")) break;
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
            } else if (msg.find(IPC_CMD_SUBSCRIBE_AUDIO) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(),
                               ^{ handle_unimplemented("subscribe_audio"); });
            } else if (msg.find(IPC_CMD_SUBSCRIBE) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(),
                               ^{ handle_unimplemented("subscribe"); });
            } else if (msg.find(IPC_CMD_START_MEDIA) != std::string::npos) {
                dispatch_async(dispatch_get_main_queue(),
                               ^{ handle_unimplemented("start_media"); });
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
    // NSApplication instance must exist. Accessory activation policy keeps this
    // helper out of the Dock and gives it no menu bar, which is what we want for
    // a process OBS launches in the background.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp run];
    return 0;
}
