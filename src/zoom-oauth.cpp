#include "zoom-oauth.h"
#include "zoom-join-decision.h"
#include "zoom-settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>
#include <obs-module.h>

#if defined(_WIN32)
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dlfcn.h>
#include <QDir>
#include <QFileInfo>

// dladdr needs an address that lives inside this plugin binary so it can report
// the plugin's own path. A pointer-to-member-function cannot be cast to void*
// (ill-formed in C++), so use the address of this file-local free function.
static void corevideo_oauth_module_anchor() {}
#endif

ZoomOAuthManager &ZoomOAuthManager::instance()
{
    static ZoomOAuthManager inst;
    return inst;
}

ZoomOAuthManager::ZoomOAuthManager(QObject *parent)
    : QObject(parent)
{
}

QString ZoomOAuthManager::random_base64url(int byte_count)
{
    QByteArray bytes;
    bytes.resize(byte_count);
    // QRandomGenerator::system() is backed by the platform CSPRNG.
    QRandomGenerator *secure_rng = QRandomGenerator::system(); // flawfinder: ignore
    for (int i = 0; i < byte_count; ++i)
        bytes[i] = static_cast<char>(secure_rng->generate() & 0xff);
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                              QByteArray::OmitTrailingEquals));
}

QString ZoomOAuthManager::pkce_challenge(const QString &verifier)
{
    const QByteArray digest = QCryptographicHash::hash(
        verifier.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toBase64(QByteArray::Base64UrlEncoding |
                                               QByteArray::OmitTrailingEquals));
}

QString ZoomOAuthManager::form_encode(const QMap<QString, QString> &fields)
{
    QUrlQuery query;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        query.addQueryItem(it.key(), it.value());
    return query.toString(QUrl::FullyEncoded);
}

static std::string redacted_tail(const QString &value)
{
    if (value.isEmpty()) return "(empty)";
    const QString tail = value.right(qMin(4, value.size()));
    return ("****" + tail).toStdString();
}

static bool is_broker_start_url(const QUrl &url)
{
    return url.scheme().startsWith("http") &&
        url.path().compare("/oauth/start", Qt::CaseInsensitive) == 0;
}

static QUrl broker_endpoint(const QString &base_url, const QString &path)
{
    QUrl url(base_url);
    url.setPath(path);
    url.setQuery(QString());
    return url;
}

static QUrl broker_base_from_authorization_url(const std::string &authorization_url)
{
    QUrl url(QString::fromStdString(authorization_url));
    url.setPath("/");
    url.setQuery(QString());
    return url;
}

bool ZoomOAuthManager::begin_authorization(QWidget *parent, QString *error)
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    QString registration_error;
    const bool registered_callback = register_url_scheme(&registration_error);
    // cppcheck-suppress knownConditionTrueFalse
    if (!registered_callback) {
        if (error) {
            *error = "Could not register the corevideo:// OAuth callback URL. " +
                     registration_error;
        }
        return false;
    }

    QUrl url = s.oauth_authorization_url.empty()
        ? QUrl("https://zoom.us/oauth/authorize")
        : QUrl(QString::fromStdString(s.oauth_authorization_url));
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        if (error) *error = "The Zoom authorization URL is not valid.";
        return false;
    }

    const bool use_broker = is_broker_start_url(url);
    QUrlQuery query(url);
    const QString configured_client_id = QString::fromStdString(s.oauth_client_id);
    QString client_id = query.queryItemValue("client_id");
    if (client_id.isEmpty())
        client_id = configured_client_id;
    if (!use_broker && client_id.isEmpty()) {
        if (error) {
            *error = "CoreVideo was built without an embedded Zoom OAuth "
                     "client ID. Rebuild with ZOOM_EMBED_OAUTH_CLIENT_ID set "
                     "to the Marketplace app's OAuth Client ID.";
        }
        return false;
    }

    m_pending_verifier = use_broker ? QString() : random_base64url(64);
    m_pending_state = random_base64url(32);
    m_pending_client_id = client_id;
    m_pending_broker_base_url = use_broker ? broker_base_from_authorization_url(
        url.toString(QUrl::RemoveQuery).toStdString()).toString() : QString();

    query.removeAllQueryItems("response_type");
    query.removeAllQueryItems("redirect_uri");
    query.removeAllQueryItems("scope");
    query.removeAllQueryItems("state");
    query.removeAllQueryItems("code_challenge");
    query.removeAllQueryItems("code_challenge_method");
    query.removeAllQueryItems("client_id");
    query.removeAllQueryItems("return_uri");
    if (use_broker) {
        query.addQueryItem("state", m_pending_state);
        query.addQueryItem("return_uri", QString::fromStdString(s.oauth_redirect_uri));
    } else {
        query.addQueryItem("response_type", "code");
        query.addQueryItem("client_id", client_id);
        query.addQueryItem("redirect_uri", QString::fromStdString(s.oauth_redirect_uri));
        if (!s.oauth_scopes.empty())
            query.addQueryItem("scope", QString::fromStdString(s.oauth_scopes));
        query.addQueryItem("state", m_pending_state);
        query.addQueryItem("code_challenge", pkce_challenge(m_pending_verifier));
        query.addQueryItem("code_challenge_method", "S256");
    }
    url.setQuery(query);

    if (!QDesktopServices::openUrl(url)) {
        if (error) *error = "Could not open the system browser.";
        return false;
    }

    if (use_broker) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Zoom OAuth authorization started through broker host=%s",
             url.host().toUtf8().constData());
    } else {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Zoom OAuth authorization started (public PKCE) client_id=%s",
             redacted_tail(client_id).c_str());
    }

    if (parent) {
        QMessageBox::information(parent, "Zoom OAuth",
            "Your browser has been opened for Zoom authorization. Return to OBS "
            "after approving CoreVideo.");
    }
    return true;
}

bool ZoomOAuthManager::parse_token_response(const QByteArray &body, QString *error)
{
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = "Zoom returned an invalid token response.";
        return false;
    }

    const QJsonObject obj = doc.object();
    const QString access_token = obj.value("access_token").toString();
    const QString refresh = obj.value("refresh_token").toString();
    if (access_token.isEmpty()) {
        if (error) {
            const QString msg = obj.value("message").toString();
            *error = msg.isEmpty() ? "Zoom did not return an access token." : msg;
        }
        return false;
    }

    ZoomPluginSettings s = ZoomPluginSettings::load();
    s.oauth_access_token = access_token.toStdString();
    if (!refresh.isEmpty())
        s.oauth_refresh_token = refresh.toStdString();
    const int expires_in = obj.value("expires_in").toInt(3600);
    s.oauth_expires_at = QDateTime::currentSecsSinceEpoch() + expires_in - 60;
    s.save();
    return true;
}

static QString oauth_error_message(const QByteArray &body,
                                   const QString &fallback)
{
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject obj = doc.object();
        const QString oauth_error = obj.value("error").toString();
        const QString reason = obj.value("reason").toString();
        // Centralized error catalog (issue #89): distinct guidance for wrong
        // environment (invalid_client), expired token (invalid_grant), invalid
        // redirect, and missing approval (invalid_scope).
        const zoom_join::ZoomJoinError category =
            zoom_join::classify_oauth_error(oauth_error.toStdString());
        if (category != zoom_join::ZoomJoinError::Unknown) {
            return QString::fromUtf8(zoom_join::join_error_guidance(category));
        }
        if (!reason.isEmpty())
            return reason;
        const QString message = obj.value("message").toString();
        if (!message.isEmpty())
            return message;
        if (!oauth_error.isEmpty())
            return oauth_error;
    }
    return body.isEmpty() ? fallback : QString::fromUtf8(body);
}

struct OAuthTokenAttemptResult {
    QByteArray response;
    int status = 0;
    QNetworkReply::NetworkError net_error = QNetworkReply::NoError;
    QString net_error_string;
};

static OAuthTokenAttemptResult post_token_request(
    QNetworkAccessManager &manager,
    const QMap<QString, QString> &fields,
    const QByteArray &authorization)
{
    QNetworkRequest request(QUrl("https://zoom.us/oauth/token"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
    if (!authorization.isEmpty())
        request.setRawHeader("Authorization", authorization);

    QEventLoop loop;
    QUrlQuery query;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        query.addQueryItem(it.key(), it.value());
    QNetworkReply *reply =
        manager.post(request, query.toString(QUrl::FullyEncoded).toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    OAuthTokenAttemptResult result;
    result.response = reply->readAll();
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.net_error = reply->error();
    result.net_error_string = reply->errorString();
    reply->deleteLater();
    return result;
}

static OAuthTokenAttemptResult post_json_request(QNetworkAccessManager &manager,
                                                 const QUrl &url,
                                                 const QJsonObject &body)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");

    QEventLoop loop;
    QNetworkReply *reply =
        manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    OAuthTokenAttemptResult result;
    result.response = reply->readAll();
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.net_error = reply->error();
    result.net_error_string = reply->errorString();
    reply->deleteLater();
    return result;
}

static bool token_attempt_succeeded(const OAuthTokenAttemptResult &result)
{
    return result.net_error == QNetworkReply::NoError &&
        result.status >= 200 && result.status < 300;
}

static OAuthTokenAttemptResult post_public_pkce_token_request(
    QNetworkAccessManager &manager,
    const QMap<QString, QString> &fields,
    const QString &oauth_client_id,
    const char *operation)
{
    QMap<QString, QString> body_fields = fields;
    body_fields.insert("client_id", oauth_client_id);

    blog(LOG_INFO,
         "[obs-zoom-plugin] Zoom OAuth %s client_id=%s",
         operation, redacted_tail(oauth_client_id).c_str());
    return post_token_request(manager, body_fields, {});
}

bool ZoomOAuthManager::handle_redirect_url(const QString &url, QString *error)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth callback received");
    const QUrl callback(url);
    const QUrlQuery query(callback);
    const QString state = query.queryItemValue("state");
    const QString code = query.queryItemValue("code");
    const QString broker_token = query.queryItemValue("broker_token");
    const QString oauth_error = query.queryItemValue("error");

    if (!oauth_error.isEmpty()) {
        if (error) *error = "Zoom OAuth failed: " + oauth_error;
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback error: %s",
             oauth_error.toUtf8().constData());
        return false;
    }
    if (m_pending_state.isEmpty() ||
        (broker_token.isEmpty() && m_pending_verifier.isEmpty())) {
        if (error) {
            *error = "Zoom returned a callback but CoreVideo has no active "
                     "sign-in in progress. This usually means OBS was "
                     "restarted, the plugin was reloaded, or the browser tab "
                     "was reused after a previous sign-in already completed. "
                     "Click Sign in with Zoom again to start a fresh flow.";
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback arrived with no active flow");
        return false;
    }
    if (state.isEmpty() || state != m_pending_state) {
        if (error) {
            *error = "Zoom OAuth callback state did not match the active "
                     "sign-in. Click Sign in with Zoom again and use the new "
                     "browser tab to approve the request.";
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback state mismatch");
        return false;
    }

    const ZoomPluginSettings s = ZoomPluginSettings::load();
    if (!broker_token.isEmpty()) {
        const QString broker_base = m_pending_broker_base_url.isEmpty()
            ? broker_base_from_authorization_url(s.oauth_authorization_url).toString()
            : m_pending_broker_base_url;
        QNetworkAccessManager manager;
        OAuthTokenAttemptResult result = post_json_request(
            manager,
            broker_endpoint(broker_base, "/oauth/redeem"),
            QJsonObject{{"broker_token", broker_token}});

        m_pending_state.clear();
        m_pending_verifier.clear();
        m_pending_client_id.clear();
        m_pending_broker_base_url.clear();

        if (!token_attempt_succeeded(result)) {
            if (error) {
                *error = "Zoom broker token redeem failed: " +
                         oauth_error_message(result.response,
                                             result.net_error_string);
            }
            blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth broker redeem failed: status=%d network=%d error=%s",
                 result.status, static_cast<int>(result.net_error),
                 result.net_error_string.toUtf8().constData());
            if (!result.response.isEmpty()) {
                blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth broker redeem response: %s",
                     QString::fromUtf8(result.response.left(512)).toUtf8().constData());
            }
            return false;
        }
        const bool ok = parse_token_response(result.response, error);
        if (ok)
            blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth broker authorization completed");
        return ok;
    }
    if (code.isEmpty()) {
        if (error) *error = "OAuth callback did not include an authorization code.";
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback missing authorization code");
        return false;
    }

    const QString client_id = m_pending_client_id.isEmpty()
        ? QString::fromStdString(s.oauth_client_id)
        : m_pending_client_id;
    QNetworkAccessManager manager;
    QMap<QString, QString> fields = {
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", QString::fromStdString(s.oauth_redirect_uri)},
        {"code_verifier", m_pending_verifier},
    };

    OAuthTokenAttemptResult result = post_public_pkce_token_request(
        manager, fields, client_id, "public token exchange");

    m_pending_state.clear();
    m_pending_verifier.clear();
    m_pending_client_id.clear();
    m_pending_broker_base_url.clear();

    if (!token_attempt_succeeded(result)) {
        if (error) {
            *error = "Zoom token exchange failed: " +
                     oauth_error_message(result.response, result.net_error_string);
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth token exchange failed: status=%d network=%d error=%s",
             result.status, static_cast<int>(result.net_error),
             result.net_error_string.toUtf8().constData());
        if (!result.response.isEmpty()) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth token exchange response: %s",
                 QString::fromUtf8(result.response.left(512)).toUtf8().constData());
        }
        return false;
    }
    const bool ok = parse_token_response(result.response, error);
    if (ok)
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth authorization completed");
    return ok;
}

// Zoom rotates refresh tokens on every use and revokes superseded
// generations (e.g. after a newer sign-in on another install). When a
// refresh comes back invalid_grant, retrying can never succeed — clear the
// dead credentials so the UI immediately offers a fresh browser sign-in
// instead of a Refresh button that fails forever.
static bool handle_dead_refresh_grant(const QByteArray &response,
                                      QString *error)
{
    if (!response.contains("invalid_grant"))
        return false;
    ZoomPluginSettings s = ZoomPluginSettings::load();
    s.oauth_access_token.clear();
    s.oauth_refresh_token.clear();
    s.oauth_expires_at = 0;
    s.save();
    blog(LOG_WARNING,
         "[obs-zoom-plugin] Zoom refresh token was revoked (invalid_grant) — "
         "cleared stored credentials; a new sign-in is required");
    if (error) {
        *error = "Your Zoom sign-in has expired (the refresh token was "
                 "revoked — this happens when a newer sign-in supersedes "
                 "it). Please click \"Sign in with Zoom\" to sign in again.";
    }
    return true;
}

bool ZoomOAuthManager::refresh_access_token_blocking(QString *error)
{
    const ZoomPluginSettings s = ZoomPluginSettings::load();
    if (s.oauth_refresh_token.empty()) {
        if (error) *error = "No Zoom OAuth refresh token is stored.";
        return false;
    }

    QNetworkAccessManager manager;
    const QUrl authorization_url(QString::fromStdString(s.oauth_authorization_url));
    if (is_broker_start_url(authorization_url)) {
        OAuthTokenAttemptResult result = post_json_request(
            manager,
            broker_endpoint(broker_base_from_authorization_url(s.oauth_authorization_url)
                                .toString(),
                            "/oauth/refresh"),
            QJsonObject{{"refresh_token",
                         QString::fromStdString(s.oauth_refresh_token)}});
        if (!token_attempt_succeeded(result)) {
            if (!result.response.isEmpty()) {
                blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth broker refresh response: %s",
                     QString::fromUtf8(result.response.left(512)).toUtf8().constData());
            }
            if (handle_dead_refresh_grant(result.response, error))
                return false;
            if (error) {
                *error = "Zoom broker token refresh failed: " +
                         oauth_error_message(result.response,
                                             result.net_error_string);
            }
            return false;
        }
        return parse_token_response(result.response, error);
    }

    const QString client_id = QString::fromStdString(s.oauth_client_id);
    QMap<QString, QString> fields = {
        {"grant_type", "refresh_token"},
        {"refresh_token", QString::fromStdString(s.oauth_refresh_token)},
    };
    OAuthTokenAttemptResult result = post_public_pkce_token_request(
        manager, fields, client_id, "public token refresh");

    if (!token_attempt_succeeded(result)) {
        if (!result.response.isEmpty()) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth token refresh response: %s",
                 QString::fromUtf8(result.response.left(512)).toUtf8().constData());
        }
        if (handle_dead_refresh_grant(result.response, error))
            return false;
        if (error) {
            *error = "Zoom token refresh failed: " +
                     oauth_error_message(result.response, result.net_error_string);
        }
        return false;
    }
    return parse_token_response(result.response, error);
}

bool ZoomOAuthManager::fetch_zak_blocking(std::string &zak,
                                          const std::string &meeting_id,
                                          QString *error)
{
    Q_UNUSED(meeting_id);

    ZoomPluginSettings s = ZoomPluginSettings::load();
    if (s.oauth_access_token.empty())
        return false;

    if (s.oauth_expires_at <= QDateTime::currentSecsSinceEpoch()) {
        if (!refresh_access_token_blocking(error))
            return false;
        s = ZoomPluginSettings::load();
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl("https://api.zoom.us/v2/users/me/token?type=zak"));
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + QByteArray::fromStdString(s.oauth_access_token));
    request.setRawHeader("Accept", "application/json");

    QEventLoop loop;
    QNetworkReply *reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray response = reply->readAll();
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QNetworkReply::NetworkError net_error = reply->error();
    QString net_error_string = reply->errorString();
    reply->deleteLater();

    if (net_error != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (error) {
            const QString details = response.isEmpty()
                ? net_error_string
                : QString::fromUtf8(response);
            *error = "Could not fetch Zoom ZAK: " + details;
        }
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(response, &parse_error);
    const QString token = doc.object().value("token").toString();
    if (parse_error.error != QJsonParseError::NoError || token.isEmpty()) {
        if (error) *error = "Zoom did not return a ZAK token.";
        return false;
    }
    zak = token.toStdString();
    return true;
}

bool ZoomOAuthManager::fetch_sdk_jwt_blocking(std::string &jwt, QString *error)
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    if (s.oauth_access_token.empty()) {
        if (error) *error = "Sign in with Zoom before requesting Meeting SDK auth.";
        return false;
    }

    if (s.oauth_expires_at <= QDateTime::currentSecsSinceEpoch()) {
        if (!refresh_access_token_blocking(error))
            return false;
        s = ZoomPluginSettings::load();
    }

    const QUrl authorization_url(QString::fromStdString(s.oauth_authorization_url));
    if (!is_broker_start_url(authorization_url)) {
        if (error) {
            *error = "Meeting SDK JWT broker is not configured for this build.";
        }
        return false;
    }

    QNetworkAccessManager manager;
    const OAuthTokenAttemptResult result = post_json_request(
        manager,
        broker_endpoint(broker_base_from_authorization_url(s.oauth_authorization_url)
                            .toString(),
                        "/oauth/sdk-jwt"),
        QJsonObject{{"access_token", QString::fromStdString(s.oauth_access_token)}});

    if (!token_attempt_succeeded(result)) {
        if (error) {
            *error = "Meeting SDK auth broker failed: " +
                     oauth_error_message(result.response,
                                         result.net_error_string);
        }
        if (!result.response.isEmpty()) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Meeting SDK auth broker response: %s",
                 QString::fromUtf8(result.response.left(512)).toUtf8().constData());
        }
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(result.response, &parse_error);
    const QString token = doc.object().value("sdk_jwt").toString();
    if (parse_error.error != QJsonParseError::NoError || token.isEmpty()) {
        if (error) *error = "Meeting SDK auth broker did not return an SDK JWT.";
        return false;
    }

    jwt = token.toStdString();
    blog(LOG_INFO, "[obs-zoom-plugin] Meeting SDK JWT fetched from broker");
    return true;
}

bool ZoomOAuthManager::register_url_scheme(QString *error)
{
#if defined(_WIN32)
    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase),
                            module_path, MAX_PATH)) {
        if (error) *error = "Could not locate the CoreVideo plugin directory.";
        return false;
    }

    const QFileInfo plugin_info(QString::fromWCharArray(module_path));
    const QString helper = plugin_info.dir().absoluteFilePath("CoreVideoOAuthCallback.exe");
    if (!QFileInfo::exists(helper)) {
        if (error) *error = "CoreVideoOAuthCallback.exe was not found beside the plugin DLL.";
        return false;
    }

    QSettings root("HKEY_CURRENT_USER\\Software\\Classes\\corevideo",
                   QSettings::NativeFormat);
    root.setValue(".", "URL:CoreVideo OAuth Callback");
    root.setValue("URL Protocol", "");
    root.setValue("shell/open/command/.",
                  QString("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(helper), "%1"));
    root.sync();
    if (root.status() != QSettings::NoError) {
        if (error) *error = "Could not write the corevideo:// URL registration.";
        return false;
    }
    return true;
#elif defined(__APPLE__)
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void *>(&corevideo_oauth_module_anchor),
                &info) ||
        !info.dli_fname) {
        if (error) *error = "Could not locate the CoreVideo plugin directory.";
        return false;
    }

    const QFileInfo plugin_info(QString::fromUtf8(info.dli_fname));
    const QString helper = plugin_info.dir().absoluteFilePath(
        "CoreVideoOAuthCallback.app");
    if (!QFileInfo::exists(helper)) {
        if (error) *error = "CoreVideoOAuthCallback.app was not found beside the plugin.";
        return false;
    }

    const QByteArray helper_path = helper.toUtf8();
    CFURLRef app_url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(helper_path.constData()),
        helper_path.size(),
        true);
    if (!app_url) {
        if (error) *error = "Could not create a URL for CoreVideoOAuthCallback.app.";
        return false;
    }

#if defined(__MAC_10_15) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 101500
    const OSStatus status = LSRegisterURL(app_url, true);
#else
    const OSStatus status = LSRegisterURL(app_url, true);
#endif
    CFRelease(app_url);
    if (status != noErr) {
        if (error) *error = QString("Could not register corevideo:// URL scheme (%1).")
            .arg(static_cast<int>(status));
        return false;
    }
    return true;
#else
    if (error) {
        *error = "Automatic custom URL scheme registration is currently implemented "
                 "for Windows and macOS. Register corevideo://oauth/callback with your OS and "
                 "forward the URL to the plugin oauth_callback control command.";
    }
    return false;
#endif
}
