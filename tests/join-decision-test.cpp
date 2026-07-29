// Unit test for the centralized Zoom join decision path (issue #89).
// Verifies the decision state machine and the error catalog without touching
// Qt, OBS, the Zoom SDK, or any real secret.
#include "zoom-join-decision.h"

#include <iostream>
#include <set>
#include <string>

using namespace zoom_join;

static int g_failures = 0;

static void check(bool cond, const std::string &name)
{
    if (!cond) {
        std::cerr << "FAIL: " << name << "\n";
        ++g_failures;
    }
}

// A production-shaped input: embedded Public Client ID, broker OAuth, public
// app key Meeting SDK auth, no operator-supplied token, holding OAuth tokens.
static JoinDecisionInputs production_inputs()
{
    JoinDecisionInputs in;
    in.oauth_client_id = "aaaabbbbccccWXYZ";
    in.oauth_authorization_url = "https://corevideo.iamfatness.us/oauth/start";
    in.meeting_sdk_auth_mode = "public_app_key";
    in.token_type = "auto_zak";
    in.has_access_token = true;
    in.has_refresh_token = true;
    return in;
}

int main()
{
    // ── Production path: broker OAuth + public app key + fetch ZAK ───────────
    {
        JoinDecisionInputs in = production_inputs();
        JoinDecisionPlan p = plan_join(in);
        check(p.oauth_client_kind == OAuthClientKind::Broker, "prod: broker oauth");
        check(p.broker_host == "corevideo.iamfatness.us", "prod: broker host");
        check(p.sdk_auth_mode == SdkAuthMode::PublicAppKey, "prod: public app key");
        check(p.public_app_key_mode, "prod: public app key mode flag");
        check(p.resolved_public_app_key == in.oauth_client_id,
              "prod: public app key resolves to oauth client id");
        check(p.zak_plan == ZakPlan::FetchViaOAuth, "prod: fetch zak");
        check(!p.needs_oauth_sign_in, "prod: no sign-in needed (has tokens)");
        check(p.blocking_error == ZoomJoinError::None, "prod: no blocking error");
    }

    // ── Fresh install: no tokens yet -> must sign in ─────────────────────────
    {
        JoinDecisionInputs in = production_inputs();
        in.has_access_token = false;
        in.has_refresh_token = false;
        JoinDecisionPlan p = plan_join(in);
        check(p.needs_oauth_sign_in, "fresh: sign-in required");
        check(p.blocking_error == ZoomJoinError::NeedsSignIn, "fresh: needs_sign_in error");
    }

    // ── Operator supplied a ZAK -> no OAuth ZAK fetch, no sign-in ────────────
    {
        JoinDecisionInputs in = production_inputs();
        in.has_access_token = false;
        in.has_refresh_token = false;
        in.token_type = "user_zak";
        in.has_typed_token = true;
        JoinDecisionPlan p = plan_join(in);
        check(p.zak_plan == ZakPlan::ProvidedByOperator, "typed zak: provided by operator");
        check(!p.needs_oauth_sign_in, "typed zak: no sign-in needed");
        check(p.blocking_error == ZoomJoinError::None, "typed zak: no blocking error");
    }

    // ── ZAK parsed from a join URL is also operator-provided ─────────────────
    {
        JoinDecisionInputs in = production_inputs();
        in.has_access_token = false;
        in.has_refresh_token = false;
        in.has_parsed_zak = true;
        JoinDecisionPlan p = plan_join(in);
        check(p.zak_plan == ZakPlan::ProvidedByOperator, "parsed zak: provided by operator");
        check(!p.needs_oauth_sign_in, "parsed zak: no sign-in needed");
    }

    // ── App-privilege token alone still needs a ZAK (matches dock) ───────────
    {
        JoinDecisionInputs in = production_inputs();
        in.has_access_token = false;
        in.has_refresh_token = false;
        in.token_type = "app_privilege_token";
        in.has_typed_token = true;
        JoinDecisionPlan p = plan_join(in);
        check(p.zak_plan == ZakPlan::FetchViaOAuth, "app-priv: still needs zak");
        check(p.needs_oauth_sign_in, "app-priv: sign-in needed");
    }

    // ── No SDK identity and engine not authed -> missing credentials ─────────
    {
        JoinDecisionInputs in;
        in.token_type = "auto_zak";
        JoinDecisionPlan p = plan_join(in);
        check(p.sdk_auth_mode == SdkAuthMode::None, "empty: no sdk auth mode");
        check(p.blocking_error == ZoomJoinError::MissingCredentials,
              "empty: missing credentials");
        check(p.oauth_client_kind == OAuthClientKind::None, "empty: no oauth client");
    }

    // ── Dev self-signed JWT fallback (SDK key/secret pair, no public key) ────
    {
        JoinDecisionInputs in;
        in.token_type = "auto_zak";
        in.has_sdk_key_secret_pair = true;
        in.oauth_client_id = "devClient1234";
        in.oauth_authorization_url = "https://zoom.us/oauth/authorize";
        in.has_access_token = true;
        in.has_refresh_token = true;
        JoinDecisionPlan p = plan_join(in);
        check(p.sdk_auth_mode == SdkAuthMode::SelfSignedJwt, "dev: self-signed jwt");
        check(p.oauth_client_kind == OAuthClientKind::PublicPkce, "dev: public pkce oauth");
        check(!p.public_app_key_mode, "dev: not public app key mode");
    }

    // ── Dev broker JWT mode: public key present but mode=broker_jwt ──────────
    {
        JoinDecisionInputs in = production_inputs();
        in.meeting_sdk_auth_mode = "broker_jwt";
        in.sdk_public_app_key = "somePublicKeyABCD"; // resolves via sdk_public_app_key
        JoinDecisionPlan p = plan_join(in);
        check(p.sdk_auth_mode == SdkAuthMode::BrokerJwt, "broker-jwt: mode");
        check(!p.public_app_key_mode, "broker-jwt: public key cleared for jwt");
    }

    // ── Engine already authenticated covers a missing SDK identity ───────────
    {
        JoinDecisionInputs in;
        in.token_type = "auto_zak";
        in.engine_already_authed = true;
        in.has_access_token = true;
        in.has_refresh_token = true;
        JoinDecisionPlan p = plan_join(in);
        check(p.blocking_error == ZoomJoinError::None,
              "authed: no missing-credentials error when already authed");
    }

    // ── Expired token with no refresh token, needing a ZAK -> expired error ──
    {
        JoinDecisionInputs in = production_inputs();
        in.has_access_token = true;
        in.has_refresh_token = false;
        in.access_token_expired = true;
        JoinDecisionPlan p = plan_join(in);
        check(p.blocking_error == ZoomJoinError::ExpiredToken, "expired: expired_token error");
    }

    // ── Broker URL detection ─────────────────────────────────────────────────
    check(is_broker_authorization_url("https://corevideo.iamfatness.us/oauth/start"),
          "broker url: https start");
    check(is_broker_authorization_url("https://host/oauth/start?x=1"),
          "broker url: start with query");
    check(!is_broker_authorization_url("https://zoom.us/oauth/authorize"),
          "broker url: authorize is not broker");
    check(!is_broker_authorization_url("corevideo://oauth/callback"),
          "broker url: custom scheme is not broker");
    check(!is_broker_authorization_url(""), "broker url: empty");
    check(url_host("https://corevideo.iamfatness.us/oauth/start") ==
              "corevideo.iamfatness.us",
          "url host extraction");

    // ── Classifier: SDK auth result, public-app-key vs jwt interpretation ────
    check(classify_sdk_auth_result("AUTHRET_JWTTOKENWRONG", true) ==
              ZoomJoinError::WrongEnvironment,
          "classify: jwt wrong in public mode -> wrong env");
    check(classify_sdk_auth_result("AUTHRET_JWTTOKENWRONG", false) ==
              ZoomJoinError::SdkAuthFailure,
          "classify: jwt wrong in jwt mode -> sdk auth failure");
    check(classify_sdk_auth_result("AUTHRET_ACCOUNTNOTENABLESDK", true) ==
              ZoomJoinError::SdkEntitlement,
          "classify: account not enabled -> entitlement");
    check(classify_sdk_auth_result("AUTHRET_NETWORKISSUE", true) ==
              ZoomJoinError::NetworkIssue,
          "classify: network issue");
    check(classify_sdk_auth_result("AUTHRET_SUCCESS", true) == ZoomJoinError::None,
          "classify: success -> none");

    // ── Classifier: meeting failure codes ────────────────────────────────────
    check(classify_meeting_fail(63) == ZoomJoinError::MissingApproval,
          "classify: 63 external meeting -> missing approval");
    check(classify_meeting_fail(505) == ZoomJoinError::OnBehalfTokenInvalid,
          "classify: 505 -> on-behalf invalid");
    check(classify_meeting_fail(504) == ZoomJoinError::NeedsSignIn,
          "classify: 504 anonymous -> needs sign in");
    check(classify_meeting_fail(999) == ZoomJoinError::Unknown,
          "classify: unknown meeting code");

    // ── Classifier: OAuth token endpoint errors ──────────────────────────────
    check(classify_oauth_error("invalid_client") == ZoomJoinError::WrongEnvironment,
          "classify: invalid_client -> wrong env");
    check(classify_oauth_error("invalid_grant") == ZoomJoinError::ExpiredToken,
          "classify: invalid_grant -> expired");
    check(classify_oauth_error("redirect_uri_mismatch") == ZoomJoinError::InvalidRedirect,
          "classify: redirect mismatch -> invalid redirect");
    check(classify_oauth_error("invalid_scope") == ZoomJoinError::MissingApproval,
          "classify: invalid_scope -> missing approval");

    // ── Every error category has DISTINCT, non-empty operator guidance ───────
    {
        const ZoomJoinError all[] = {
            ZoomJoinError::MissingCredentials, ZoomJoinError::NeedsSignIn,
            ZoomJoinError::WrongEnvironment,   ZoomJoinError::ExpiredToken,
            ZoomJoinError::InvalidRedirect,    ZoomJoinError::MissingApproval,
            ZoomJoinError::SdkAuthFailure,     ZoomJoinError::SdkEntitlement,
            ZoomJoinError::NetworkIssue,       ZoomJoinError::OnBehalfTokenInvalid,
            ZoomJoinError::Unknown,
        };
        std::set<std::string> messages;
        std::set<std::string> ids;
        for (ZoomJoinError e : all) {
            const std::string msg = join_error_guidance(e);
            const std::string id = join_error_id(e);
            check(!msg.empty(), std::string("guidance non-empty: ") + id);
            check(messages.insert(msg).second,
                  std::string("guidance distinct: ") + id);
            check(ids.insert(id).second, std::string("id distinct: ") + id);
        }
    }

    // ── Log block: contains only redacted tails, never the full identity ─────
    {
        JoinDecisionInputs in = production_inputs();
        JoinDecisionPlan p = plan_join(in);
        const std::string block = format_join_decision_path(in, p);
        check(block.rfind("[join-decision]", 0) == 0, "log: has tag prefix");
        check(block.find("****WXYZ") != std::string::npos, "log: redacted tail present");
        check(block.find(in.oauth_client_id) == std::string::npos,
              "log: full client id NOT present");
        check(block.find("sdk_auth_mode=public_app_key") != std::string::npos,
              "log: sdk auth mode present");
        check(block.find("oauth_flow=broker") != std::string::npos, "log: oauth flow present");
        check(block.find("broker=corevideo.iamfatness.us") != std::string::npos,
              "log: broker host present");
        check(block.find("zak=fetch_via_oauth") != std::string::npos, "log: zak plan present");
        check(block.find("blocking_error=none") != std::string::npos,
              "log: blocking error present");
    }

    // ── Redaction helper edge cases ──────────────────────────────────────────
    check(redacted_tail("") == "(empty)", "redact: empty");
    check(redacted_tail("ab") == "****", "redact: short");
    check(redacted_tail("abcdef") == "****cdef", "redact: normal");

    if (g_failures == 0) {
        std::cout << "join-decision-test: all checks passed\n";
        return 0;
    }
    std::cerr << "join-decision-test: " << g_failures << " check(s) failed\n";
    return 1;
}
