#include "zoom-settings.h"
#include "zoom-credentials.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <obs-frontend-api.h>
#include <util/config-file.h>

static constexpr const char *SECTION = "ZoomPlugin";

static QByteArray base64url(const QByteArray &data)
{
    return data.toBase64(QByteArray::Base64UrlEncoding |
                         QByteArray::OmitTrailingEquals);
}

ZoomPluginSettings ZoomPluginSettings::load()
{
    config_t *cfg = obs_frontend_get_global_config();
    ZoomPluginSettings s;

    // User-supplied values take precedence; fall back to compiled-in credentials.
    const char *key    = config_get_string(cfg, SECTION, "SdkKey");
    const char *secret = config_get_string(cfg, SECTION, "SdkSecret");
    const char *jwt    = config_get_string(cfg, SECTION, "JwtToken");

    s.sdk_key    = (key    && *key)    ? key    : kEmbeddedSdkKey;
    s.sdk_secret = (secret && *secret) ? secret : kEmbeddedSdkSecret;
    s.jwt_token  = (jwt    && *jwt)    ? jwt    : kEmbeddedJwtToken;

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

    return s;
}

std::string ZoomPluginSettings::resolved_jwt_token() const
{
    if (!jwt_token.empty()) return jwt_token;
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
    config_save_safe(cfg, "tmp", nullptr);
}
