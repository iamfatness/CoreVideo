#pragma once
//
// zoom-join-decision.h — the single, auditable "join decision path" for the
// CoreVideo Zoom integration (GitHub issue #89).
//
// The production path is fixed and enforced:
//   * Sign-in: Zoom Public Client OAuth + PKCE (or the HTTPS broker that wraps
//     it). No desktop client secret, no user-entered credentials.
//   * Join:    the signed-in user's ZAK, fetched over OAuth.
//   * Meeting SDK auth: the Marketplace Public Client ID as AuthContext
//     publicAppKey where supported.
//
// Historically the code picked between OAuth / ZAK / public app key / SDK JWT /
// broker in several scattered places, which is exactly what kept regressing.
// This header centralizes every branch point into one pure function,
// plan_join(), plus one error-string catalog. Both are free of Qt / OBS / Zoom
// SDK dependencies so they can be unit-tested without secrets or a live meeting
// (see tests/join-decision-test.cpp).
//
// Rules:
//   * NEVER put a full secret, token, client id, ZAK, or app key in a log line.
//     Use redacted_tail() — last 4 chars only.
//   * Keep every operator-facing message in join_error_guidance() so the
//     messages stay distinct and testable.
//
#include <cstdint>
#include <string>

namespace zoom_join {

// ── Redaction ────────────────────────────────────────────────────────────────
// Last 4 characters only, masked. Used for every identifier that touches a log.
inline std::string redacted_tail(const std::string &value)
{
    if (value.empty()) return "(empty)";
    if (value.size() <= 4) return "****";
    return "****" + value.substr(value.size() - 4);
}

// ── Decision enums ───────────────────────────────────────────────────────────

// How the Meeting SDK (in the ZoomObsEngine child process) will authenticate.
enum class SdkAuthMode {
    None,          // no auth material available and engine not already authed
    PublicAppKey,  // AuthContext.publicAppKey = Marketplace Public Client ID (production)
    SelfSignedJwt, // dev-only: HS256 JWT signed from an SDK key/secret pair
    BrokerJwt,     // dev/staging: SDK JWT minted by the CoreVideo broker
};

// How the sign-in / OAuth authorization request is issued.
enum class OAuthClientKind {
    None,        // no OAuth client id configured at all
    PublicPkce,  // direct Public Client OAuth + PKCE against zoom.us
    Broker,      // HTTPS broker /oauth/start wraps the PKCE flow (production)
};

// Where the join ZAK / owner-context token comes from.
enum class ZakPlan {
    NotNeeded,          // operator supplied a ZAK / on-behalf token already
    ProvidedByOperator, // typed or parsed-from-URL token will be used
    FetchViaOAuth,      // fetch the signed-in user's ZAK over OAuth before join
};

// ── Error catalog ────────────────────────────────────────────────────────────
// One enum, one message function. Each value maps to DISTINCT operator guidance
// so support can tell environments / tokens / redirects / approval / SDK-auth
// failures apart without reading logs.
enum class ZoomJoinError {
    None,
    MissingCredentials,   // build has no SDK auth material and engine not authed
    NeedsSignIn,          // must complete Zoom OAuth sign-in first
    WrongEnvironment,     // client id / app key not valid for this Zoom env
    ExpiredToken,         // OAuth token expired and could not be refreshed
    InvalidRedirect,      // redirect URI not on the Marketplace allow list
    MissingApproval,      // app not published/approved or account not entitled
    SdkAuthFailure,       // Meeting SDK rejected the JWT / key-secret
    SdkEntitlement,       // account tier not enabled for the Meeting SDK
    NetworkIssue,         // transient network / timeout / busy
    OnBehalfTokenInvalid, // on-behalf token bad or mismatched for the meeting
    Unknown,
};

// Short, stable identifier — used in logs and tests (never localized).
inline const char *join_error_id(ZoomJoinError e)
{
    switch (e) {
    case ZoomJoinError::None:                 return "none";
    case ZoomJoinError::MissingCredentials:   return "missing_credentials";
    case ZoomJoinError::NeedsSignIn:          return "needs_sign_in";
    case ZoomJoinError::WrongEnvironment:     return "wrong_environment";
    case ZoomJoinError::ExpiredToken:         return "expired_token";
    case ZoomJoinError::InvalidRedirect:      return "invalid_redirect";
    case ZoomJoinError::MissingApproval:      return "missing_approval";
    case ZoomJoinError::SdkAuthFailure:       return "sdk_auth_failure";
    case ZoomJoinError::SdkEntitlement:       return "sdk_entitlement";
    case ZoomJoinError::NetworkIssue:         return "network_issue";
    case ZoomJoinError::OnBehalfTokenInvalid: return "on_behalf_token_invalid";
    case ZoomJoinError::Unknown:              return "unknown";
    }
    return "unknown";
}

// Operator-facing, actionable guidance. DISTINCT per category (issue #89 AC).
inline const char *join_error_guidance(ZoomJoinError e)
{
    switch (e) {
    case ZoomJoinError::None:
        return "";
    case ZoomJoinError::MissingCredentials:
        return "This CoreVideo build has no Zoom Meeting SDK identity embedded. "
               "Install a published build (or, for a dev build, set the embedded "
               "Public Client ID) before joining.";
    case ZoomJoinError::NeedsSignIn:
        return "Sign in with Zoom before joining. CoreVideo needs your Zoom "
               "account's ZAK to join with owner/host context.";
    case ZoomJoinError::WrongEnvironment:
        return "Zoom rejected the app identity for this environment. The OAuth "
               "Client ID / Meeting SDK Public Client ID this build was compiled "
               "with does not match an active Marketplace app, or it belongs to a "
               "different Zoom environment (dev vs beta vs production). Confirm you "
               "are running the build for this environment.";
    case ZoomJoinError::ExpiredToken:
        return "Your Zoom sign-in has expired and could not be refreshed. Click "
               "Sign in with Zoom again to get a fresh token, then retry the join.";
    case ZoomJoinError::InvalidRedirect:
        return "Zoom rejected the OAuth redirect URI. The redirect URI this build "
               "uses is not on the Marketplace app's allow list. Add it to the "
               "app's OAuth allow list (publisher action) and try again.";
    case ZoomJoinError::MissingApproval:
        return "Zoom accepted your sign-in but this app is not approved to join "
               "this meeting. The Marketplace app must be published (or beta/prod "
               "access granted for this account), and Meeting SDK / Embed enabled, "
               "for the host account you are joining.";
    case ZoomJoinError::SdkAuthFailure:
        return "The Zoom Meeting SDK rejected authentication. The embedded Public "
               "Client ID (or dev SDK JWT) was not accepted. Confirm the app "
               "identity is valid and enabled for Meeting SDK / Embed.";
    case ZoomJoinError::SdkEntitlement:
        return "This Zoom account is not enabled for the Meeting SDK. Ask the "
               "account admin to enable Meeting SDK / Embed access, then retry.";
    case ZoomJoinError::NetworkIssue:
        return "A network problem interrupted Zoom authentication (timeout or "
               "service busy). Check connectivity and try the join again.";
    case ZoomJoinError::OnBehalfTokenInvalid:
        return "Zoom rejected the on-behalf token: it is invalid or does not match "
               "this meeting. Re-generate the token or use Zoom sign-in instead.";
    case ZoomJoinError::Unknown:
        return "Zoom authentication failed for an unrecognized reason. See the "
               "OBS log and support bundle join-decision block for the raw code.";
    }
    return "";
}

// ── Classifiers ──────────────────────────────────────────────────────────────
// Map raw Zoom result identifiers onto the catalog. Kept string/int based so the
// engine (which speaks the SDK) can report a name/code over IPC and the plugin
// maps it here — no SDK headers needed to test this.

// Meeting SDK AuthResult, keyed by the AUTHRET_* name the engine reports.
// public_app_key_mode changes the interpretation of key/secret/jwt rejections:
// in public-app-key mode those almost always mean the Public Client ID is not
// enabled for Embed on this environment (wrong env), not a bad secret.
inline ZoomJoinError classify_sdk_auth_result(const std::string &authret_name,
                                              bool public_app_key_mode)
{
    if (authret_name == "AUTHRET_SUCCESS")
        return ZoomJoinError::None;
    if (authret_name == "AUTHRET_ACCOUNTNOTSUPPORT" ||
        authret_name == "AUTHRET_ACCOUNTNOTENABLESDK")
        return ZoomJoinError::SdkEntitlement;
    if (authret_name == "AUTHRET_NETWORKISSUE" ||
        authret_name == "AUTHRET_OVERTIME" ||
        authret_name == "AUTHRET_SERVICE_BUSY")
        return ZoomJoinError::NetworkIssue;
    if (authret_name == "AUTHRET_CLIENT_INCOMPATIBLE")
        return ZoomJoinError::WrongEnvironment;
    if (authret_name == "AUTHRET_KEYORSECRETEMPTY" ||
        authret_name == "AUTHRET_KEYORSECRETWRONG" ||
        authret_name == "AUTHRET_JWTTOKENWRONG")
        return public_app_key_mode ? ZoomJoinError::WrongEnvironment
                                   : ZoomJoinError::SdkAuthFailure;
    if (authret_name == "AUTHRET_LIMIT_EXCEEDED_EXCEPTION")
        return ZoomJoinError::SdkAuthFailure;
    return ZoomJoinError::SdkAuthFailure;
}

// Meeting join failure (MEETING_STATUS_FAILED iResult code).
inline ZoomJoinError classify_meeting_fail(int code)
{
    switch (code) {
    case 63:  // MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING
    case 60:  // MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING
    case 62:  // MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN
    case 64:  // MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN
        return ZoomJoinError::MissingApproval;
    case 500: // MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR
    case 502: // MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR
    case 503: // MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF
    case 505: // MEETING_FAIL_ON_BEHALF_TOKEN_INVALID
    case 506: // MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING
        return ZoomJoinError::OnBehalfTokenInvalid;
    case 504: // MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING
        return ZoomJoinError::NeedsSignIn;
    case 82:  // MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING
    case 23:  // MEETING_FAIL_ENFORCE_LOGIN
        return ZoomJoinError::NeedsSignIn;
    default:
        return ZoomJoinError::Unknown;
    }
}

// OAuth token endpoint error ("error" field of the token response body).
inline ZoomJoinError classify_oauth_error(const std::string &oauth_error_code)
{
    if (oauth_error_code == "invalid_client")
        return ZoomJoinError::WrongEnvironment;
    if (oauth_error_code == "invalid_grant")
        return ZoomJoinError::ExpiredToken;
    if (oauth_error_code == "redirect_uri_mismatch" ||
        oauth_error_code == "invalid_redirect_uri")
        return ZoomJoinError::InvalidRedirect;
    if (oauth_error_code == "invalid_scope")
        return ZoomJoinError::MissingApproval;
    return ZoomJoinError::Unknown;
}

// ── Decision planner ─────────────────────────────────────────────────────────

struct JoinDecisionInputs {
    // App identity (raw values — only tails are ever logged).
    std::string oauth_client_id;          // Marketplace OAuth / Public Client ID
    std::string oauth_authorization_url;  // broker /oauth/start URL, or empty
    std::string meeting_sdk_auth_mode;    // "public_app_key" | "broker_jwt"
    std::string sdk_public_app_key;       // configured public app key (dev)
    bool        has_sdk_key_secret_pair = false; // dev self-sign material present
    bool        has_embedded_jwt        = false; // dev static JWT present
    bool        engine_already_authed   = false; // engine SDKAuth already succeeded

    // OAuth token state.
    bool        has_access_token   = false;
    bool        has_refresh_token  = false;
    bool        access_token_expired = false;

    // Join-token selection from the dock.
    std::string token_type;                 // "auto_zak"|"user_zak"|"app_privilege_token"
    bool        has_typed_token         = false;
    bool        has_parsed_on_behalf    = false; // on-behalf token parsed from URL
    bool        has_parsed_zak          = false; // ZAK parsed from URL
};

struct JoinDecisionPlan {
    OAuthClientKind oauth_client_kind = OAuthClientKind::None;
    std::string     broker_host;              // host of broker URL, if broker
    SdkAuthMode     sdk_auth_mode = SdkAuthMode::None;
    std::string     resolved_public_app_key;  // raw; caller redacts for logs
    bool            public_app_key_mode = false;
    ZakPlan         zak_plan = ZakPlan::NotNeeded;
    bool            needs_oauth_sign_in = false;
    ZoomJoinError   blocking_error = ZoomJoinError::None;
};

// True if the configured authorization URL is a broker /oauth/start endpoint.
inline bool is_broker_authorization_url(const std::string &url)
{
    if (url.rfind("http", 0) != 0) return false; // must be http(s)
    // Find the path component after the authority.
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return false;
    auto path = url.substr(path_start);
    const auto q = path.find_first_of("?#");
    if (q != std::string::npos) path = path.substr(0, q);
    // Case-insensitive compare against "/oauth/start".
    const std::string want = "/oauth/start";
    if (path.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != want[i]) return false;
    }
    return true;
}

// Host portion of an http(s) URL (for logging the broker without the path).
inline std::string url_host(const std::string &url)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {};
    const auto host_start = scheme_end + 3;
    const auto host_end = url.find_first_of("/?#", host_start);
    return url.substr(host_start,
                      host_end == std::string::npos ? std::string::npos
                                                    : host_end - host_start);
}

// Mirrors ZoomPluginSettings::resolved_meeting_sdk_public_app_key().
inline std::string resolve_public_app_key(const JoinDecisionInputs &in)
{
    if (in.meeting_sdk_auth_mode == "public_app_key" && !in.oauth_client_id.empty())
        return in.oauth_client_id;
    return in.sdk_public_app_key;
}

// The one auditable decision. Pure: same inputs -> same plan, no side effects.
inline JoinDecisionPlan plan_join(const JoinDecisionInputs &in)
{
    JoinDecisionPlan plan;

    // 1. OAuth client kind.
    if (is_broker_authorization_url(in.oauth_authorization_url)) {
        plan.oauth_client_kind = OAuthClientKind::Broker;
        plan.broker_host = url_host(in.oauth_authorization_url);
    } else if (!in.oauth_client_id.empty()) {
        plan.oauth_client_kind = OAuthClientKind::PublicPkce;
    } else {
        plan.oauth_client_kind = OAuthClientKind::None;
    }

    // 2. Meeting SDK auth mode (mirrors dock + settings resolution order).
    plan.resolved_public_app_key = resolve_public_app_key(in);
    const bool use_public_app_key = !plan.resolved_public_app_key.empty();
    plan.public_app_key_mode = use_public_app_key;
    const bool broker_jwt_mode = in.meeting_sdk_auth_mode == "broker_jwt";
    if (use_public_app_key && broker_jwt_mode) {
        // dev/staging: a broker mints an SDK JWT, public key is cleared at join.
        plan.sdk_auth_mode = SdkAuthMode::BrokerJwt;
        plan.public_app_key_mode = false;
    } else if (use_public_app_key) {
        plan.sdk_auth_mode = SdkAuthMode::PublicAppKey;   // production
    } else if (in.has_embedded_jwt || in.has_sdk_key_secret_pair) {
        plan.sdk_auth_mode = SdkAuthMode::SelfSignedJwt;  // dev fallback
    } else {
        plan.sdk_auth_mode = SdkAuthMode::None;
    }

    // 3. ZAK plan. Owner/host context needs a ZAK unless the operator already
    //    supplied a ZAK or an on-behalf token (app-privilege token alone still
    //    needs a ZAK, matching the existing dock behaviour).
    const bool user_zak_present =
        in.has_parsed_zak ||
        (in.has_typed_token && in.token_type == "user_zak");
    const bool on_behalf_present =
        in.has_parsed_on_behalf ||
        (in.has_typed_token && in.token_type != "user_zak" &&
         in.token_type != "app_privilege_token");
    const bool needs_zak = !user_zak_present && !on_behalf_present;
    if (!needs_zak)
        plan.zak_plan = ZakPlan::ProvidedByOperator;
    else
        plan.zak_plan = ZakPlan::FetchViaOAuth;

    // 4. Sign-in requirement: need a ZAK but hold no OAuth tokens at all.
    plan.needs_oauth_sign_in =
        needs_zak && !in.has_access_token && !in.has_refresh_token;

    // 5. Blocking errors (checked in priority order).
    if (plan.sdk_auth_mode == SdkAuthMode::None && !in.engine_already_authed) {
        plan.blocking_error = ZoomJoinError::MissingCredentials;
    } else if (plan.needs_oauth_sign_in) {
        plan.blocking_error = ZoomJoinError::NeedsSignIn;
    } else if (needs_zak && in.access_token_expired && !in.has_refresh_token) {
        plan.blocking_error = ZoomJoinError::ExpiredToken;
    } else {
        plan.blocking_error = ZoomJoinError::None;
    }

    return plan;
}

// ── Log rendering ────────────────────────────────────────────────────────────

inline const char *sdk_auth_mode_id(SdkAuthMode m)
{
    switch (m) {
    case SdkAuthMode::None:          return "none";
    case SdkAuthMode::PublicAppKey:  return "public_app_key";
    case SdkAuthMode::SelfSignedJwt: return "self_signed_jwt";
    case SdkAuthMode::BrokerJwt:     return "broker_jwt";
    }
    return "none";
}

inline const char *oauth_client_kind_id(OAuthClientKind k)
{
    switch (k) {
    case OAuthClientKind::None:       return "none";
    case OAuthClientKind::PublicPkce: return "public_pkce";
    case OAuthClientKind::Broker:     return "broker";
    }
    return "none";
}

inline const char *zak_plan_id(ZakPlan z)
{
    switch (z) {
    case ZakPlan::NotNeeded:          return "not_needed";
    case ZakPlan::ProvidedByOperator: return "provided_by_operator";
    case ZakPlan::FetchViaOAuth:      return "fetch_via_oauth";
    }
    return "not_needed";
}

// The single coherent "join decision path" log block (issue #89). Never
// contains a full secret — only redacted tails, kinds, and boolean flags.
inline std::string format_join_decision_path(const JoinDecisionInputs &in,
                                             const JoinDecisionPlan &plan)
{
    std::string out = "[join-decision]";
    out += " oauth_flow=";      out += oauth_client_kind_id(plan.oauth_client_kind);
    out += " oauth_client=";    out += redacted_tail(in.oauth_client_id);
    if (plan.oauth_client_kind == OAuthClientKind::Broker) {
        out += " broker=";
        out += plan.broker_host.empty() ? "(unknown)" : plan.broker_host;
    }
    out += " sdk_auth_mode=";   out += sdk_auth_mode_id(plan.sdk_auth_mode);
    out += " public_app_key=";  out += redacted_tail(plan.resolved_public_app_key);
    out += " zak=";             out += zak_plan_id(plan.zak_plan);
    out += " token_type=";      out += in.token_type.empty() ? "auto_zak"
                                                              : in.token_type;
    out += " have_access_token=";  out += in.has_access_token ? "1" : "0";
    out += " have_refresh_token="; out += in.has_refresh_token ? "1" : "0";
    out += " token_expired=";      out += in.access_token_expired ? "1" : "0";
    out += " sign_in_required=";   out += plan.needs_oauth_sign_in ? "1" : "0";
    out += " blocking_error=";     out += join_error_id(plan.blocking_error);
    return out;
}

} // namespace zoom_join
