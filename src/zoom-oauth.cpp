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

bool ZoomOAuthManager::begin_authorization(QWidget *parent, QString *error)
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    QUrl url = s.oauth_authorization_url.empty()
        ? QUrl("https://zoom.us/oauth/authorize")
        : QUrl(QString::fromStdString(s.oauth_authorization_url));
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        if (error) *error = "The Zoom authorization URL is not valid.";
        return false;
    }

    QUrlQuery query(url);
    QString client_id = QString::fromStdString(s.oauth_client_id);
    if (client_id.isEmpty())
        client_id = query.queryItemValue("client_id");
    if (client_id.isEmpty()) {
        if (error) *error = "Enter the Zoom OAuth Client ID first.";
        return false;
    }

    m_pending_verifier = random_base64url(64);
    m_pending_state = random_base64url(32);
    m_pending_client_id = client_id;

    if (s.oauth_client_id.empty()) {
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

bool ZoomOAuthManager::handle_redirect_url(const QString &url, QString *error)
{
    const QUrl callback(url);
    const QUrlQuery query(callback);
    const QString state = query.queryItemValue("state");
    const QString code = query.queryItemValue("code");
    const QString oauth_error = query.queryItemValue("error");

    if (!oauth_error.isEmpty()) {
        if (error) *error = "Zoom OAuth failed: " + oauth_error;
        return false;
    }
    if (code.isEmpty()) {
        if (error) *error = "OAuth callback did not include an authorization code.";
        return false;
    }
    if (state.isEmpty() || state != m_pending_state || m_pending_verifier.isEmpty()) {
        if (error) *error = "OAuth callback state did not match the active login.";
        return false;
    }

    const ZoomPluginSettings s = ZoomPluginSettings::load();
    const QString client_id = m_pending_client_id.isEmpty()
        ? QString::fromStdString(s.oauth_client_id)
        : m_pending_client_id;
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl("https://zoom.us/oauth/token"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
    const QString body = form_encode({
        {"grant_type", "authorization_code"},
        {"client_id", client_id},
        {"code", code},
        {"redirect_uri", QString::fromStdString(s.oauth_redirect_uri)},
        {"code_verifier", m_pending_verifier},
    });

    QEventLoop loop;
    QNetworkReply *reply = manager.post(request, body.toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray response = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError net_error = reply->error();
    reply->deleteLater();

    m_pending_state.clear();
    m_pending_verifier.clear();
    m_pending_client_id.clear();

    if (net_error != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (error) *error = "Zoom token exchange failed: " + QString::fromUtf8(response);
        return false;
    }
    const bool ok = parse_token_response(response, error);
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
    QNetworkRequest request(QUrl("https://zoom.us/oauth/token"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
    const QString body = form_encode({
        {"grant_type", "refresh_token"},
        {"client_id", QString::fromStdString(s.oauth_client_id)},
        {"refresh_token", QString::fromStdString(s.oauth_refresh_token)},
    });

    QEventLoop loop;
    QNetworkReply *reply = manager.post(request, body.toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray response = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError net_error = reply->error();
    reply->deleteLater();

    if (net_error != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (error) *error = "Zoom token refresh failed: " + QString::fromUtf8(response);
        return false;
    }
    return parse_token_response(response, error);
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
    reply->deleteLater();

    if (net_error != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (error) *error = "Could not fetch Zoom ZAK: " + QString::fromUtf8(response);
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
#else
    if (error) {
        *error = "Automatic custom URL scheme registration is currently implemented "
                 "for Windows. Register corevideo://oauth/callback with your OS and "
                 "forward the URL to the plugin oauth_callback control command.";
    }
    return false;
#endif
}
