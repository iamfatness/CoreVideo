#include "zoom-settings.h"
#include "zoom-credentials.h"
#include <cstdint>
#include <QByteArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <obs-frontend-api.h>
#include <util/base.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <util/config-file.h>
#if defined(_WIN32)
#include <windows.h>
#include <dpapi.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

static constexpr const char *SECTION = "ZoomPlugin";
static QByteArray base64url(const QByteArray &data)
{
    return data.toBase64(QByteArray::Base64UrlEncoding |
                         QByteArray::OmitTrailingEquals);
}

static bool looks_like_jwt(const std::string &token)
{
    const size_t first_dot = token.find('.');
    if (first_dot == std::string::npos || first_dot == 0) return false;

    const size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos || second_dot == first_dot + 1)
        return false;

    return second_dot + 1 < token.size() &&
        token.find('.', second_dot + 1) == std::string::npos;
}

static bool has_embedded_value(const char *value)
{
    return value && *value;
}

#if !defined(_WIN32) && !defined(__APPLE__)
// No OS-level secret store is wired up on this platform yet (Windows uses
// DPAPI, macOS uses the Keychain below; Linux libsecret is tracked as
// follow-up work - see README.md "Security"). Until then, OAuth tokens are
// written to OBS's global.ini in plaintext, so warn loudly instead of failing
// silently. Logged once per process.
static void warn_plaintext_token_storage_once()
{
    static bool warned = false;
    if (warned) return;
    warned = true;
    blog(LOG_WARNING,
         "[obs-zoom-plugin] SECURITY: OAuth tokens are stored WITHOUT "
         "OS-level encryption on this platform (DPAPI token protection is "
         "Windows-only; macOS uses the Keychain). Access/refresh tokens are "
         "written in plaintext to OBS's global.ini under the [ZoomPlugin] "
         "section. Anyone with filesystem or config-file access can read "
         "them. See the Security section of README.md.");
}
#endif

#if defined(__APPLE__)
// macOS Keychain-backed secret storage. Secrets live in the login Keychain
// under the service "CoreVideo OBS Plugin", keyed by an account name (the
// config key). global.ini stores only an empty placeholder, so tokens are
// never written to disk in plaintext. Failures are loud and never fall back to
// plaintext: a failed store leaves no token (re-auth required), and a failed
// read returns empty (re-auth required) rather than a stale/guessed value.
static constexpr const char *kKeychainService = "CoreVideo OBS Plugin";

static bool keychain_set_secret(const char *account, const std::string &secret)
{
    CFStringRef svc = CFStringCreateWithCString(nullptr, kKeychainService,
                                                kCFStringEncodingUTF8);
    CFStringRef acc = CFStringCreateWithCString(nullptr, account,
                                                kCFStringEncodingUTF8);
    const void *qkeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void *qvals[] = {kSecClassGenericPassword, svc, acc};
    CFDictionaryRef query = CFDictionaryCreate(
        nullptr, qkeys, qvals, 3, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    OSStatus st;
    bool ok;
    if (secret.empty()) {
        // No value to store: remove any existing item. Not-found is success.
        st = SecItemDelete(query);
        ok = (st == errSecSuccess || st == errSecItemNotFound);
    } else {
        CFDataRef data = CFDataCreate(
            nullptr, reinterpret_cast<const UInt8 *>(secret.data()),
            static_cast<CFIndex>(secret.size()));
        const void *ukeys[] = {kSecValueData};
        const void *uvals[] = {data};
        CFDictionaryRef upd = CFDictionaryCreate(
            nullptr, ukeys, uvals, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        st = SecItemUpdate(query, upd);
        if (st == errSecItemNotFound) {
            CFMutableDictionaryRef add =
                CFDictionaryCreateMutableCopy(nullptr, 0, query);
            CFDictionarySetValue(add, kSecValueData, data);
            st = SecItemAdd(add, nullptr);
            CFRelease(add);
        }
        ok = (st == errSecSuccess);
        CFRelease(upd);
        CFRelease(data);
    }
    if (!ok) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] SECURITY: Keychain store failed for '%s' "
             "(OSStatus %d). Token was NOT persisted - re-authentication will "
             "be required. The token is never written to disk in plaintext.",
             account, static_cast<int>(st));
    }
    CFRelease(query);
    CFRelease(acc);
    CFRelease(svc);
    return ok;
}

static std::string keychain_get_secret(const char *account)
{
    CFStringRef svc = CFStringCreateWithCString(nullptr, kKeychainService,
                                                kCFStringEncodingUTF8);
    CFStringRef acc = CFStringCreateWithCString(nullptr, account,
                                                kCFStringEncodingUTF8);
    const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                          kSecReturnData, kSecMatchLimit};
    const void *vals[] = {kSecClassGenericPassword, svc, acc, kCFBooleanTrue,
                          kSecMatchLimitOne};
    CFDictionaryRef query = CFDictionaryCreate(
        nullptr, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(query, &result);
    std::string out;
    if (st == errSecSuccess && result &&
        CFGetTypeID(result) == CFDataGetTypeID()) {
        CFDataRef data = static_cast<CFDataRef>(result);
        out.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                   static_cast<size_t>(CFDataGetLength(data)));
    } else if (st != errSecItemNotFound) {
        // A hard error (not merely "no item") - surface it; caller re-auths.
        blog(LOG_ERROR,
             "[obs-zoom-plugin] SECURITY: Keychain read failed for '%s' "
             "(OSStatus %d). Re-authentication will be required.",
             account, static_cast<int>(st));
    }
    if (result)
        CFRelease(result);
    CFRelease(query);
    CFRelease(acc);
    CFRelease(svc);
    return out;
}
#endif

// Transform a secret into the value stored in global.ini.
//   Windows: DPAPI-encrypted base64 blob (byte-identical to before).
//   macOS:   stored in the login Keychain under `account`; returns empty so
//            the ini holds only a placeholder (never plaintext).
//   Linux:   plaintext (loud one-time warning).
// `account` is the config key; used only on macOS as the Keychain account.
static std::string protect_secret(const std::string &secret, const char *account)
{
#if defined(_WIN32)
    (void)account;
    if (secret.empty()) return {};
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(secret.data()));
    in.cbData = static_cast<DWORD>(secret.size());
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"CoreVideo Zoom OAuth token", nullptr, nullptr,
                          nullptr, 0, &out)) {
        return {};
    }
    QByteArray bytes(reinterpret_cast<const char *>(out.pbData),
                     static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return bytes.toBase64().toStdString();
#elif defined(__APPLE__)
    // Store (or clear) in the Keychain; the ini keeps only an empty
    // placeholder. keychain_set_secret logs loudly on failure.
    keychain_set_secret(account, secret);
    return {};
#else
    (void)account;
    if (secret.empty()) return {};
    warn_plaintext_token_storage_once();
    return secret;
#endif
}

static std::string unprotect_secret(const char *stored, const char *account)
{
#if defined(_WIN32)
    (void)account;
    if (!stored || !*stored) return {};
    const QByteArray encrypted = QByteArray::fromBase64(QByteArray(stored));
    if (encrypted.isEmpty()) return {};
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()));
    in.cbData = static_cast<DWORD>(encrypted.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return {};
    }
    std::string secret(reinterpret_cast<const char *>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return secret;
#elif defined(__APPLE__)
    // The real token lives in the Keychain; the ini value is only a
    // placeholder. A missing/corrupt Keychain item yields empty (re-auth),
    // never a silent stale value.
    (void)stored;
    return keychain_get_secret(account);
#else
    (void)account;
    if (!stored || !*stored) return {};
    warn_plaintext_token_storage_once();
    return stored;
#endif
}

// OBS 31 split the frontend "global" config into app and user scopes and
// deprecated obs_frontend_get_global_config — which also logs a DEPRECATION
// warning on EVERY call, flooding logs on OBS 31+. The deprecated accessor
// maps to the app config (the same global.ini this plugin has always written),
// so obs_frontend_get_app_config is the drop-in successor with zero settings
// migration. It does not exist on OBS 30, and referencing it statically would
// stop the plugin from loading there, so resolve it at runtime and keep the
// old accessor as the OBS 30 fallback.
static config_t *frontend_settings_config()
{
    using config_fn = config_t *(*)(void);
    static const config_fn app_config_fn = []() -> config_fn {
#if defined(_WIN32)
        HMODULE mod = GetModuleHandleA("obs-frontend-api.dll");
        return mod ? reinterpret_cast<config_fn>(reinterpret_cast<void *>(
                         GetProcAddress(mod, "obs_frontend_get_app_config")))
                   : nullptr;
#else
        return reinterpret_cast<config_fn>(
            dlsym(RTLD_DEFAULT, "obs_frontend_get_app_config"));
#endif
    }();
    if (app_config_fn) return app_config_fn();
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    return obs_frontend_get_global_config();
    QT_WARNING_POP
}

ZoomPluginSettings ZoomPluginSettings::load()
{
    config_t *cfg = frontend_settings_config();
    ZoomPluginSettings s;

    const bool embedded_public_app_key =
        has_embedded_value(kEmbeddedMeetingSdkPublicAppKey);
    const bool embedded_oauth_client_id =
        has_embedded_value(kEmbeddedOAuthClientId);
    const bool embedded_oauth_authorization_url =
        has_embedded_value(kEmbeddedOAuthAuthorizationUrl);

    // Published builds use compiled-in app identity. Local config is only a
    // development fallback when the build did not embed the identity.
    const char *key    = config_get_string(cfg, SECTION, "SdkKey");
    const char *secret = config_get_string(cfg, SECTION, "SdkSecret");
    const char *meeting_sdk_client_id =
        config_get_string(cfg, SECTION, "MeetingSdkClientId");
    const char *meeting_sdk_client_secret =
        config_get_string(cfg, SECTION, "MeetingSdkClientSecret");
    const char *meeting_sdk_public_app_key =
        config_get_string(cfg, SECTION, "MeetingSdkPublicAppKey");
    const char *meeting_sdk_auth_mode =
        config_get_string(cfg, SECTION, "MeetingSdkAuthMode");
    const char *jwt    = config_get_string(cfg, SECTION, "JwtToken");
    // OAuthClientId is a dev-only override; production builds use the embedded
    // ZOOM_EMBED_OAUTH_CLIENT_ID value. PublicClientId is a legacy key from
    // earlier builds and is read once for migration, then cleared on save.
    const char *oauth_client_id = config_get_string(cfg, SECTION, "OAuthClientId");
    const char *oauth_legacy_public_client_id =
        config_get_string(cfg, SECTION, "PublicClientId");
    const char *oauth_authorization_url = config_get_string(cfg, SECTION, "OAuthAuthorizationUrl");
    const char *oauth_redirect_uri = config_get_string(cfg, SECTION, "OAuthRedirectUri");
    const char *oauth_scopes = config_get_string(cfg, SECTION, "OAuthScopes");

    if (embedded_public_app_key) {
        s.sdk_public_app_key = kEmbeddedMeetingSdkPublicAppKey;
        s.sdk_key.clear();
        s.sdk_secret.clear();
        s.jwt_token.clear();
    } else {
        s.sdk_key = (meeting_sdk_client_id && *meeting_sdk_client_id)
            ? meeting_sdk_client_id
            : ((key && *key) ? key : kEmbeddedSdkKey);

        const std::string protected_meeting_secret =
            unprotect_secret(meeting_sdk_client_secret, "MeetingSdkClientSecret");
        if (!protected_meeting_secret.empty()) {
            s.sdk_secret = protected_meeting_secret;
        } else {
            s.sdk_secret = (secret && *secret) ? secret : kEmbeddedSdkSecret;
        }
        s.jwt_token = (jwt && *jwt) ? jwt : kEmbeddedJwtToken;
        s.sdk_public_app_key = (meeting_sdk_public_app_key &&
                                *meeting_sdk_public_app_key)
            ? meeting_sdk_public_app_key
            : "";
    }
    if (meeting_sdk_auth_mode && *meeting_sdk_auth_mode)
        s.meeting_sdk_auth_mode = meeting_sdk_auth_mode;
    if (embedded_public_app_key)
        s.meeting_sdk_auth_mode = "public_app_key";
    if (s.meeting_sdk_auth_mode != "public_app_key" &&
        s.meeting_sdk_auth_mode != "broker_jwt") {
        s.meeting_sdk_auth_mode = "public_app_key";
    }

    if (embedded_oauth_client_id) {
        s.oauth_client_id = kEmbeddedOAuthClientId;
    } else if (oauth_client_id && *oauth_client_id) {
        s.oauth_client_id = oauth_client_id;
    } else if (oauth_legacy_public_client_id && *oauth_legacy_public_client_id) {
        s.oauth_client_id = oauth_legacy_public_client_id;
    } else {
        s.oauth_client_id = kEmbeddedOAuthClientId;
    }
    s.oauth_authorization_url = embedded_oauth_authorization_url
        ? kEmbeddedOAuthAuthorizationUrl
        : (oauth_authorization_url ? oauth_authorization_url : "");
    if (oauth_redirect_uri && *oauth_redirect_uri)
        s.oauth_redirect_uri = oauth_redirect_uri;
    if (oauth_scopes && *oauth_scopes)
        s.oauth_scopes = oauth_scopes;
    s.oauth_access_token = unprotect_secret(
        config_get_string(cfg, SECTION, "OAuthAccessToken"), "OAuthAccessToken");
    s.oauth_refresh_token = unprotect_secret(
        config_get_string(cfg, SECTION, "OAuthRefreshToken"), "OAuthRefreshToken");
    s.oauth_expires_at = static_cast<int64_t>(
        config_get_int(cfg, SECTION, "OAuthExpiresAt"));

    s.control_server_port = static_cast<uint16_t>(
        config_get_uint(cfg, SECTION, "ControlServerPort"));
    if (s.control_server_port == 0) s.control_server_port = 19870;
    s.osc_server_port = static_cast<uint16_t>(
        config_get_uint(cfg, SECTION, "OscServerPort"));
    if (s.osc_server_port == 0) s.osc_server_port = 19871;
    const char *control_token = config_get_string(cfg, SECTION, "ControlToken");
    s.control_token = control_token ? control_token : "";
    s.hw_accel_mode = static_cast<HwAccelMode>(
        config_get_int(cfg, SECTION, "HwAccelMode"));
    if (config_has_user_value(cfg, SECTION, "CheckForUpdatesOnStartup"))
        s.check_for_updates_on_startup =
            config_get_int(cfg, SECTION, "CheckForUpdatesOnStartup") != 0;

    // Reconnect policy — defaults come from the ZoomReconnectPolicy struct.
    const int rc_enabled = config_get_int(cfg, SECTION, "ReconnectEnabled");
    if (rc_enabled >= 0) s.reconnect_policy.enabled = (rc_enabled != 0);
    const int rc_max = config_get_int(cfg, SECTION, "ReconnectMaxAttempts");
    if (rc_max > 0) s.reconnect_policy.max_attempts = rc_max;
    const int rc_base = config_get_int(cfg, SECTION, "ReconnectBaseDelayMs");
    if (rc_base > 0) s.reconnect_policy.base_delay_ms = rc_base;
    const int rc_max_ms = config_get_int(cfg, SECTION, "ReconnectMaxDelayMs");
    if (rc_max_ms > 0) s.reconnect_policy.max_delay_ms = rc_max_ms;
    const int rc_crash = config_get_int(cfg, SECTION, "ReconnectOnCrash");
    if (rc_crash >= 0) s.reconnect_policy.on_engine_crash = (rc_crash != 0);
    const int rc_disc = config_get_int(cfg, SECTION, "ReconnectOnDisconnect");
    if (rc_disc >= 0) s.reconnect_policy.on_disconnect = (rc_disc != 0);
    const int rc_auth = config_get_int(cfg, SECTION, "ReconnectOnAuthFail");
    if (rc_auth >= 0) s.reconnect_policy.on_auth_fail = (rc_auth != 0);

    const char *last_id   = config_get_string(cfg, SECTION, "LastMeetingId");
    const char *last_name = config_get_string(cfg, SECTION, "LastDisplayName");
    s.last_meeting_id   = last_id   ? last_id   : "";
    s.last_display_name = last_name ? last_name : "";
    s.last_was_webinar  = config_get_int(cfg, SECTION, "LastWasWebinar") != 0;
    s.hide_participants_without_video =
        config_get_int(cfg, SECTION, "HideParticipantsWithoutVideo") != 0;
    s.audio_delay_ms = static_cast<uint32_t>(
        config_get_int(cfg, SECTION, "AudioDelayMs"));
    if (s.audio_delay_ms > 500) s.audio_delay_ms = 500;

    const char *iso_output_dir = config_get_string(cfg, SECTION, "IsoOutputDir");
    const char *iso_ffmpeg_path = config_get_string(cfg, SECTION, "IsoFfmpegPath");
    const char *iso_video_encoder = config_get_string(cfg, SECTION, "IsoVideoEncoder");
    s.iso_output_dir = iso_output_dir ? iso_output_dir : "";
    if (iso_ffmpeg_path && *iso_ffmpeg_path)
        s.iso_ffmpeg_path = iso_ffmpeg_path;
    if (iso_video_encoder && *iso_video_encoder)
        s.iso_video_encoder = iso_video_encoder;
    if (config_has_user_value(cfg, SECTION, "IsoRecordProgram"))
        s.iso_record_program =
            config_get_int(cfg, SECTION, "IsoRecordProgram") != 0;

    const char *talkback_source = config_get_string(cfg, SECTION, "TalkbackSource");
    s.talkback_source = talkback_source ? talkback_source : "";
    if (config_has_user_value(cfg, SECTION, "TalkbackLatch"))
        s.talkback_latch = config_get_int(cfg, SECTION, "TalkbackLatch") != 0;
    if (config_has_user_value(cfg, SECTION, "TalkbackProbeExpanded"))
        s.talkback_probe_expanded =
            config_get_int(cfg, SECTION, "TalkbackProbeExpanded") != 0;

    const int speaker_sensitivity =
        config_get_int(cfg, SECTION, "SpeakerSensitivityMs");
    if (config_has_user_value(cfg, SECTION, "SpeakerSensitivityMs") &&
        speaker_sensitivity >= 0)
        s.speaker_sensitivity_ms = static_cast<uint32_t>(speaker_sensitivity);
    const int speaker_hold = config_get_int(cfg, SECTION, "SpeakerHoldMs");
    if (config_has_user_value(cfg, SECTION, "SpeakerHoldMs") &&
        speaker_hold >= 0)
        s.speaker_hold_ms = static_cast<uint32_t>(speaker_hold);
    if (config_has_user_value(cfg, SECTION, "SpeakerRequireVideo"))
        s.speaker_require_video =
            config_get_int(cfg, SECTION, "SpeakerRequireVideo") != 0;
    const int64_t speaker_exclude_1 =
        config_get_int(cfg, SECTION, "SpeakerExcludeParticipant1");
    if (config_has_user_value(cfg, SECTION, "SpeakerExcludeParticipant1") &&
        speaker_exclude_1 >= 0 && speaker_exclude_1 <= UINT32_MAX)
        s.speaker_exclude_participant_1 =
            static_cast<uint32_t>(speaker_exclude_1);
    const int64_t speaker_exclude_2 =
        config_get_int(cfg, SECTION, "SpeakerExcludeParticipant2");
    if (config_has_user_value(cfg, SECTION, "SpeakerExcludeParticipant2") &&
        speaker_exclude_2 >= 0 && speaker_exclude_2 <= UINT32_MAX)
        s.speaker_exclude_participant_2 =
            static_cast<uint32_t>(speaker_exclude_2);

    return s;
}

std::string ZoomPluginSettings::resolved_meeting_sdk_public_app_key() const
{
    if (meeting_sdk_auth_mode == "public_app_key" && !oauth_client_id.empty())
        return oauth_client_id;
    return sdk_public_app_key;
}

std::string ZoomPluginSettings::resolved_jwt_token() const
{
    if (!jwt_token.empty() && looks_like_jwt(jwt_token)) return jwt_token;
    if (sdk_key.empty() || sdk_secret.empty()) return {};

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 iat = now - 30;
    const qint64 exp = now + 60 * 60 * 2;

    QJsonObject header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";

    QJsonObject payload;
    payload["appKey"] = QString::fromStdString(sdk_key);
    payload["iat"] = iat;
    payload["exp"] = exp;
    payload["tokenExp"] = exp;

    const QByteArray encoded_header = base64url(
        QJsonDocument(header).toJson(QJsonDocument::Compact));
    const QByteArray encoded_payload = base64url(
        QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QByteArray signing_input = encoded_header + "." + encoded_payload;
    const QByteArray signature = QMessageAuthenticationCode::hash(
        signing_input,
        QByteArray::fromStdString(sdk_secret),
        QCryptographicHash::Sha256);

    return (signing_input + "." + base64url(signature)).toStdString();
}

bool ZoomPluginSettings::use_broker_sdk_jwt() const
{
    return meeting_sdk_auth_mode == "broker_jwt";
}

void ZoomPluginSettings::save() const
{
    config_t *cfg = frontend_settings_config();
    const bool embedded_public_app_key =
        has_embedded_value(kEmbeddedMeetingSdkPublicAppKey);
    const bool embedded_oauth_client_id =
        has_embedded_value(kEmbeddedOAuthClientId);
    const bool embedded_oauth_authorization_url =
        has_embedded_value(kEmbeddedOAuthAuthorizationUrl);
    const std::string saved_sdk_key =
        embedded_public_app_key ? std::string() : sdk_key;
    const std::string saved_sdk_secret =
        embedded_public_app_key ? std::string() : sdk_secret;
    const std::string saved_jwt =
        embedded_public_app_key ? std::string() : jwt_token;
    const std::string saved_public_app_key = embedded_public_app_key
        ? kEmbeddedMeetingSdkPublicAppKey
        : sdk_public_app_key;
    const std::string saved_oauth_client_id = embedded_oauth_client_id
        ? kEmbeddedOAuthClientId
        : oauth_client_id;
    const std::string saved_oauth_authorization_url =
        embedded_oauth_authorization_url
            ? kEmbeddedOAuthAuthorizationUrl
            : oauth_authorization_url;

    config_set_string(cfg, SECTION, "SdkKey",            saved_sdk_key.c_str());
    config_set_string(cfg, SECTION, "SdkSecret",         saved_sdk_secret.c_str());
    config_set_string(cfg, SECTION, "MeetingSdkClientId", saved_sdk_key.c_str());
    config_set_string(cfg, SECTION, "MeetingSdkClientSecret",
                      protect_secret(saved_sdk_secret, "MeetingSdkClientSecret").c_str());
    config_set_string(cfg, SECTION, "MeetingSdkPublicAppKey",
                      saved_public_app_key.c_str());
    config_set_string(cfg, SECTION, "MeetingSdkAuthMode",
                      embedded_public_app_key ? "public_app_key" :
                          meeting_sdk_auth_mode.c_str());
    config_set_string(cfg, SECTION, "JwtToken",          saved_jwt.c_str());
    config_set_string(cfg, SECTION, "OAuthClientId",     saved_oauth_client_id.c_str());
    // Legacy keys cleared so older builds don't resurrect stale values. Public
    // PKCE is the only supported runtime mode; secrets must never be shipped
    // in a desktop binary.
    config_set_string(cfg, SECTION, "PublicClientId",    "");
    config_set_string(cfg, SECTION, "OAuthClientSecret", "");
    config_set_int   (cfg, SECTION, "OAuthUseClientSecret", 0);
    config_set_string(cfg, SECTION, "OAuthAuthorizationUrl",
                      saved_oauth_authorization_url.c_str());
    config_set_string(cfg, SECTION, "OAuthRedirectUri",  oauth_redirect_uri.c_str());
    config_set_string(cfg, SECTION, "OAuthScopes",       oauth_scopes.c_str());
    config_set_string(cfg, SECTION, "OAuthAccessToken",
                      protect_secret(oauth_access_token, "OAuthAccessToken").c_str());
    config_set_string(cfg, SECTION, "OAuthRefreshToken",
                      protect_secret(oauth_refresh_token, "OAuthRefreshToken").c_str());
    config_set_int   (cfg, SECTION, "OAuthExpiresAt",
                      static_cast<int>(oauth_expires_at));
    config_set_uint  (cfg, SECTION, "ControlServerPort", control_server_port);
    config_set_uint  (cfg, SECTION, "OscServerPort",     osc_server_port);
    config_set_string(cfg, SECTION, "ControlToken",      control_token.c_str());
    config_set_int   (cfg, SECTION, "HwAccelMode",       static_cast<int>(hw_accel_mode));
    config_set_int   (cfg, SECTION, "CheckForUpdatesOnStartup",
                      check_for_updates_on_startup ? 1 : 0);
    config_set_int   (cfg, SECTION, "ReconnectEnabled",       reconnect_policy.enabled ? 1 : 0);
    config_set_int   (cfg, SECTION, "ReconnectMaxAttempts",   reconnect_policy.max_attempts);
    config_set_int   (cfg, SECTION, "ReconnectBaseDelayMs",   reconnect_policy.base_delay_ms);
    config_set_int   (cfg, SECTION, "ReconnectMaxDelayMs",    reconnect_policy.max_delay_ms);
    config_set_int   (cfg, SECTION, "ReconnectOnCrash",       reconnect_policy.on_engine_crash ? 1 : 0);
    config_set_int   (cfg, SECTION, "ReconnectOnDisconnect",  reconnect_policy.on_disconnect ? 1 : 0);
    config_set_int   (cfg, SECTION, "ReconnectOnAuthFail",    reconnect_policy.on_auth_fail ? 1 : 0);
    config_set_string(cfg, SECTION, "LastMeetingId",          last_meeting_id.c_str());
    config_set_string(cfg, SECTION, "LastDisplayName",        last_display_name.c_str());
    config_set_int   (cfg, SECTION, "LastWasWebinar",         last_was_webinar ? 1 : 0);
    config_set_int   (cfg, SECTION, "HideParticipantsWithoutVideo",
                      hide_participants_without_video ? 1 : 0);
    config_set_int   (cfg, SECTION, "AudioDelayMs", audio_delay_ms);
    config_set_string(cfg, SECTION, "IsoOutputDir",           iso_output_dir.c_str());
    config_set_string(cfg, SECTION, "IsoFfmpegPath",          iso_ffmpeg_path.c_str());
    config_set_string(cfg, SECTION, "IsoVideoEncoder",        iso_video_encoder.c_str());
    config_set_int   (cfg, SECTION, "IsoRecordProgram",       iso_record_program ? 1 : 0);
    config_set_string(cfg, SECTION, "TalkbackSource",        talkback_source.c_str());
    config_set_int   (cfg, SECTION, "TalkbackLatch",         talkback_latch ? 1 : 0);
    config_set_int   (cfg, SECTION, "TalkbackProbeExpanded", talkback_probe_expanded ? 1 : 0);
    config_set_int   (cfg, SECTION, "SpeakerSensitivityMs",
                      static_cast<int>(speaker_sensitivity_ms));
    config_set_int   (cfg, SECTION, "SpeakerHoldMs",
                      static_cast<int>(speaker_hold_ms));
    config_set_int   (cfg, SECTION, "SpeakerRequireVideo",
                      speaker_require_video ? 1 : 0);
    // Zoom user IDs are uint32 and routinely exceed INT_MAX; store the full
    // 64-bit value so high-ID participants persist (a signed-int cast made
    // their exclusion silently fail to save).
    config_set_int   (cfg, SECTION, "SpeakerExcludeParticipant1",
                      static_cast<int64_t>(speaker_exclude_participant_1));
    config_set_int   (cfg, SECTION, "SpeakerExcludeParticipant2",
                      static_cast<int64_t>(speaker_exclude_participant_2));
    config_save_safe(cfg, "tmp", nullptr);
}
