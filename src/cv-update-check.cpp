#include "cv-update-check.h"
#include "cv-version-compare.h"
#include "obs-zoom-version.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr const char *kReleasesApiUrl =
    "https://api.github.com/repos/iamfatness/CoreVideo/releases/latest";
constexpr const char *kReleasesPageUrl =
    "https://github.com/iamfatness/CoreVideo/releases/latest";
}

CvUpdateChecker &CvUpdateChecker::instance()
{
    static CvUpdateChecker s_instance;
    return s_instance;
}

CvUpdateChecker::CvUpdateChecker(QObject *parent) : QObject(parent) {}

void CvUpdateChecker::check_once()
{
    if (m_started)
        return;
    m_started = true;

    if (!m_manager)
        m_manager = new QNetworkAccessManager(this);

    // A plain, unauthenticated GET with no query parameters or headers that
    // identify this install - only what GitHub already sees for any
    // anonymous HTTP request (IP, User-Agent). No meeting data, tokens, or
    // stable identifiers are sent. See docs/policies/privacy-policy.md.
    QNetworkRequest request{QUrl(QString::fromLatin1(kReleasesApiUrl))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "CoreVideo-OBS-Plugin");

    QNetworkReply *reply = m_manager->get(request);
    connect(reply, &QNetworkReply::finished, this,
            &CvUpdateChecker::on_reply_finished);
}

void CvUpdateChecker::on_reply_finished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    // Fail totally silently: no blog(), no UI, nothing - on any network,
    // HTTP, or JSON error. An offline or rate-limited operator must see
    // zero noise from this check.
    if (reply->error() != QNetworkReply::NoError)
        return;

    const QByteArray body = reply->readAll();
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString tag = obj.value(QStringLiteral("tag_name")).toString();
    if (tag.isEmpty())
        return;

    if (!cv_is_newer_version(OBS_ZOOM_PLUGIN_VERSION, tag.toStdString()))
        return;

    QString html_url = obj.value(QStringLiteral("html_url")).toString();
    if (html_url.isEmpty())
        html_url = QString::fromLatin1(kReleasesPageUrl);

    m_has_update = true;
    m_update_tag = tag;
    m_update_url = html_url;
    emit update_available(tag, html_url);
}
