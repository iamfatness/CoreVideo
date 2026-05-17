#include "zoom-settings.h"
#include "zoom-credentials.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#if defined(_WIN32)
#include <windows.h>
#include <dpapi.h>
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

static std::string protect_secret(const std::string &secret)
{
    if (secret.empty()) return {};
#if defined(_WIN32)
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
#else
    return secret;
#endif
}

static std::string unprotect_secret(const char *stored)
{
    if (!stored || !*stored) return {};
#if defined(_WIN32)
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
#else
    return stored;
#endif
}

ZoomPluginSettings ZoomPluginSettings::load()
{
    config_t *cfg = obs_frontend_get_global_config();
    ZoomPluginSettings s;

    // User-supplied values take precedence; fall back to compiled-in credentials.
    const char *key    = config_get_string(cfg, SECTION, "SdkKey");
    const char *secret = config_get_string(cfg, SECTION, "SdkSecret");
    const char *jwt    = config_get_string(cfg, SECTION, "JwtToken");
    const char *oauth_client_id = config_get_string(cfg, SECTION, "OAuthClientId");
    const char *oauth_client_secret = config_get_string(cfg, SECTION, "OAuthClientSecret");
    const int oauth_use_client_secret =
        config_get_int(cfg, SECTION, "OAuthUseClientSecret");
    const char *oauth_authorization_url = config_get_string(cfg, SECTION, "OAuthAuthorizationUrl");
    const char *oauth_redirect_uri = config_get_string(cfg, SECTION, "OAuthRedirectUri");
    const char *oauth_scopes = config_get_string(cfg, SECTION, "OAuthScopes");

    s.sdk_key    = (key    && *key)    ? key    : kEmbeddedSdkKey;
    s.sdk_secret = (secret && *secret) ? secret : kEmbeddedSdkSecret;
    s.jwt_token  = (jwt    && *jwt)    ? jwt    : kEmbeddedJwtToken;
    s.oauth_client_id = (oauth_client_id && *oauth_client_id)
        ? oauth_client_id
        : kEmbeddedOAuthClientId;
    s.oauth_client_secret = unprotect_secret(oauth_client_secret);
    s.oauth_use_client_secret = oauth_use_client_secret != 0;
    s.oauth_authorization_url = oauth_authorization_url ? oauth_authorization_url : "";
    if (oauth_redirect_uri && *oauth_redirect_uri)
        s.oauth_redirect_uri = oauth_redirect_uri;
    if (oauth_scopes && *oauth_scopes)
        s.oauth_scopes = oauth_scopes;
    s.oauth_access_token = unprotect_secret(
        config_get_string(cfg, SECTION, "OAuthAccessToken"));
    s.oauth_refresh_token = unprotect_secret(
        config_get_string(cfg, SECTION, "OAuthRefreshToken"));
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

    const char *iso_output_dir = config_get_string(cfg, SECTION, "IsoOutputDir");
    const char *iso_ffmpeg_path = config_get_string(cfg, SECTION, "IsoFfmpegPath");
    s.iso_output_dir = iso_output_dir ? iso_output_dir : "";
    if (iso_ffmpeg_path && *iso_ffmpeg_path)
        s.iso_ffmpeg_path = iso_ffmpeg_path;
    if (config_has_user_value(cfg, SECTION, "IsoRecordProgram"))
        s.iso_record_program =
            config_get_int(cfg, SECTION, "IsoRecordProgram") != 0;

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

    return s;
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

void ZoomPluginSettings::save() const
{
    config_t *cfg = obs_frontend_get_global_config();
    config_set_string(cfg, SECTION, "SdkKey",            sdk_key.c_str());
    config_set_string(cfg, SECTION, "SdkSecret",         sdk_secret.c_str());
    config_set_string(cfg, SECTION, "JwtToken",          jwt_token.c_str());
    config_set_string(cfg, SECTION, "OAuthClientId",     oauth_client_id.c_str());
    config_set_string(cfg, SECTION, "OAuthClientSecret",
                      protect_secret(oauth_client_secret).c_str());
    config_set_int   (cfg, SECTION, "OAuthUseClientSecret",
                      oauth_use_client_secret ? 1 : 0);
    config_set_string(cfg, SECTION, "OAuthAuthorizationUrl",
                      oauth_authorization_url.c_str());
    config_set_string(cfg, SECTION, "OAuthRedirectUri",  oauth_redirect_uri.c_str());
    config_set_string(cfg, SECTION, "OAuthScopes",       oauth_scopes.c_str());
    config_set_string(cfg, SECTION, "OAuthAccessToken",
                      protect_secret(oauth_access_token).c_str());
    config_set_string(cfg, SECTION, "OAuthRefreshToken",
                      protect_secret(oauth_refresh_token).c_str());
    config_set_int   (cfg, SECTION, "OAuthExpiresAt",
                      static_cast<int>(oauth_expires_at));
    config_set_uint  (cfg, SECTION, "ControlServerPort", control_server_port);
    config_set_uint  (cfg, SECTION, "OscServerPort",     osc_server_port);
    config_set_string(cfg, SECTION, "ControlToken",      control_token.c_str());
    config_set_int   (cfg, SECTION, "HwAccelMode",       static_cast<int>(hw_accel_mode));
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
    config_set_string(cfg, SECTION, "IsoOutputDir",           iso_output_dir.c_str());
    config_set_string(cfg, SECTION, "IsoFfmpegPath",          iso_ffmpeg_path.c_str());
    config_set_int   (cfg, SECTION, "IsoRecordProgram",       iso_record_program ? 1 : 0);
    config_set_int   (cfg, SECTION, "SpeakerSensitivityMs",
                      static_cast<int>(speaker_sensitivity_ms));
    config_set_int   (cfg, SECTION, "SpeakerHoldMs",
                      static_cast<int>(speaker_hold_ms));
    config_set_int   (cfg, SECTION, "SpeakerRequireVideo",
                      speaker_require_video ? 1 : 0);
    config_save_safe(cfg, "tmp", nullptr);
}
