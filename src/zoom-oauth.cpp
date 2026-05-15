#include "zoom-oauth.h"
#include "zoom-settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 0; i < byte_count; ++i)
        bytes[i] = static_cast<char>(rng->generate() & 0xff);
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

static QByteArray oauth_basic_auth(const QString &client_id,
                                   const QString &client_secret)
{
    if (client_secret.isEmpty())
        return "Basic " + client_id.toUtf8().toBase64();
    return "Basic " + (client_id + ":" + client_secret).toUtf8().toBase64();
}

bool ZoomOAuthManager::begin_authorization(QWidget *parent, QString *error)
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    QString registration_error;
    if (!register_url_scheme(&registration_error)) {
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

    QUrlQuery query(url);
    const QString url_client_id = query.queryItemValue("client_id");
    QString client_id = url_client_id.isEmpty()
        ? QString::fromStdString(s.oauth_client_id)
        : url_client_id;
    if (client_id.isEmpty()) {
        if (error) *error = "Enter the Zoom OAuth Client ID first.";
        return false;
    }

    m_pending_verifier = random_base64url(64);
    m_pending_state = random_base64url(32);
    m_pending_client_id = client_id;

    if (s.oauth_client_id != client_id.toStdString()) {
        s.oauth_client_id = client_id.toStdString();
        s.save();
    }

    query.removeAllQueryItems("response_type");
    query.removeAllQueryItems("client_id");
    query.removeAllQueryItems("redirect_uri");
    query.removeAllQueryItems("scope");
    query.removeAllQueryItems("state");
    query.removeAllQueryItems("code_challenge");
    query.removeAllQueryItems("code_challenge_method");
    query.addQueryItem("response_type", "code");
    query.addQueryItem("client_id", client_id);
    query.addQueryItem("redirect_uri", QString::fromStdString(s.oauth_redirect_uri));
    if (!s.oauth_scopes.empty())
        query.addQueryItem("scope", QString::fromStdString(s.oauth_scopes));
    query.addQueryItem("state", m_pending_state);
    query.addQueryItem("code_challenge", pkce_challenge(m_pending_verifier));
    query.addQueryItem("code_challenge_method", "S256");
    url.setQuery(query);

    if (!QDesktopServices::openUrl(url)) {
        if (error) *error = "Could not open the system browser.";
        return false;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth authorization started");

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
    const QString access = obj.value("access_token").toString();
    const QString refresh = obj.value("refresh_token").toString();
    if (access.isEmpty()) {
        if (error) {
            const QString msg = obj.value("message").toString();
            *error = msg.isEmpty() ? "Zoom did not return an access token." : msg;
        }
        return false;
    }

    ZoomPluginSettings s = ZoomPluginSettings::load();
    s.oauth_access_token = access.toStdString();
    if (!refresh.isEmpty())
        s.oauth_refresh_token = refresh.toStdString();
    const int expires_in = obj.value("expires_in").toInt(3600);
    s.oauth_expires_at = QDateTime::currentSecsSinceEpoch() + expires_in - 60;
    s.save();
    return true;
}

static QString oauth_error_message(const QByteArray &body,
                                   const QString &fallback,
                                   bool using_client_secret)
{
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject obj = doc.object();
        const QString oauth_error = obj.value("error").toString();
        const QString reason = obj.value("reason").toString();
        if (oauth_error == "invalid_client") {
            if (using_client_secret) {
                return "Zoom rejected the OAuth client credentials. For "
                       "confidential OAuth, verify the regular Client ID and "
                       "Client Secret from the same Zoom Marketplace app, then "
                       "save settings and authorize again.";
            }
            return "Zoom rejected the OAuth client. For public PKCE, enable "
                   "Use Public Client OAuth in the Zoom Marketplace app and "
                   "use the separate Public Client ID, not the regular Client "
                   "ID. If you intentionally use a confidential client, enable "
                   "Use Client Secret in CoreVideo and re-enter the client secret.";
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

static bool token_attempt_succeeded(const OAuthTokenAttemptResult &result)
{
    return result.net_error == QNetworkReply::NoError &&
        result.status >= 200 && result.status < 300;
}

static bool token_attempt_invalid_client(const OAuthTokenAttemptResult &result)
{
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(result.response, &parse_error);
    return parse_error.error == QJsonParseError::NoError && doc.isObject() &&
        doc.object().value("error").toString() == "invalid_client";
}

bool ZoomOAuthManager::handle_redirect_url(const QString &url, QString *error)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth callback received");
    const QUrl callback(url);
    const QUrlQuery query(callback);
    const QString state = query.queryItemValue("state");
    const QString code = query.queryItemValue("code");
    const QString oauth_error = query.queryItemValue("error");

    if (!oauth_error.isEmpty()) {
        if (error) *error = "Zoom OAuth failed: " + oauth_error;
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback error: %s",
             oauth_error.toUtf8().constData());
        return false;
    }
    if (code.isEmpty()) {
        if (error) *error = "OAuth callback did not include an authorization code.";
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback missing authorization code");
        return false;
    }
    if (state.isEmpty() || state != m_pending_state || m_pending_verifier.isEmpty()) {
        if (error) *error = "OAuth callback state did not match the active login.";
        blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth callback state mismatch");
        return false;
    }

    const ZoomPluginSettings s = ZoomPluginSettings::load();
    const QString client_id = m_pending_client_id.isEmpty()
        ? QString::fromStdString(s.oauth_client_id)
        : m_pending_client_id;
    const QString client_secret = s.oauth_use_client_secret
        ? QString::fromStdString(s.oauth_client_secret)
        : QString();
    QNetworkAccessManager manager;
    QMap<QString, QString> fields = {
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", QString::fromStdString(s.oauth_redirect_uri)},
        {"code_verifier", m_pending_verifier},
    };

    OAuthTokenAttemptResult result = post_token_request(
        manager, fields, oauth_basic_auth(client_id, client_secret));

    if (!s.oauth_use_client_secret && token_attempt_invalid_client(result)) {
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth public token exchange retry with client_id form field");
        fields.insert("client_id", client_id);
        result = post_token_request(manager, fields, {});
    }

    if (!s.oauth_use_client_secret && token_attempt_invalid_client(result)) {
        blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth public token exchange retry with Basic auth and client_id form field");
        result = post_token_request(manager, fields, oauth_basic_auth(client_id, {}));
    }

    m_pending_state.clear();
    m_pending_verifier.clear();
    m_pending_client_id.clear();

    if (!token_attempt_succeeded(result)) {
        if (error) {
            *error = "Zoom token exchange failed: " +
                     oauth_error_message(result.response, result.net_error_string,
                                         s.oauth_use_client_secret);
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

bool ZoomOAuthManager::refresh_access_token_blocking(QString *error)
{
    const ZoomPluginSettings s = ZoomPluginSettings::load();
    if (s.oauth_refresh_token.empty()) {
        if (error) *error = "No Zoom OAuth refresh token is stored.";
        return false;
    }

    QNetworkAccessManager manager;
    const QString client_id = QString::fromStdString(s.oauth_client_id);
    const QString client_secret = s.oauth_use_client_secret
        ? QString::fromStdString(s.oauth_client_secret)
        : QString();
    QMap<QString, QString> fields = {
        {"grant_type", "refresh_token"},
        {"refresh_token", QString::fromStdString(s.oauth_refresh_token)},
    };
    OAuthTokenAttemptResult result = post_token_request(
        manager, fields, oauth_basic_auth(client_id, client_secret));

    if (!s.oauth_use_client_secret && token_attempt_invalid_client(result)) {
        fields.insert("client_id", client_id);
        result = post_token_request(manager, fields, {});
    }

    if (!s.oauth_use_client_secret && token_attempt_invalid_client(result)) {
        result = post_token_request(manager, fields, oauth_basic_auth(client_id, {}));
    }

    if (!token_attempt_succeeded(result)) {
        if (error) {
            *error = "Zoom token refresh failed: " +
                     oauth_error_message(result.response, result.net_error_string,
                                         s.oauth_use_client_secret);
        }
        if (!result.response.isEmpty()) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth token refresh response: %s",
                 QString::fromUtf8(result.response.left(512)).toUtf8().constData());
        }
        return false;
    }
    return parse_token_response(result.response, error);
}

bool ZoomOAuthManager::fetch_zak_blocking(std::string &zak, QString *error)
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    if (s.oauth_access_token.empty() || s.oauth_client_id.empty())
        return false;

    if (s.oauth_expires_at <= QDateTime::currentSecsSinceEpoch()) {
        if (!refresh_access_token_blocking(error))
            return false;
        s = ZoomPluginSettings::load();
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl("https://api.zoom.us/v2/users/me/zak"));
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + QByteArray::fromStdString(s.oauth_access_token));
    request.setRawHeader("Accept", "application/json");

    QEventLoop loop;
    QNetworkReply *reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray response = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError net_error = reply->error();
    const QString net_error_string = reply->errorString();
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
    if (!dladdr(reinterpret_cast<const void *>(&ZoomOAuthManager::register_url_scheme),
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
