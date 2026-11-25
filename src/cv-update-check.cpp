#include "cv-update-check.h"
#include "cv-version-compare.h"
#include "obs-zoom-version.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr const char *kReleasesApiUrl =
    "https://api.github.com/repos/iamfatness/CoreVideo/releases?per_page=100";
#if defined(Q_OS_WIN) && (defined(Q_PROCESSOR_X86_64) || defined(Q_PROCESSOR_ARM_64))
constexpr auto kReleasePlatform = CvReleasePlatform::Windows;
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
constexpr auto kReleasePlatform = CvReleasePlatform::MacOS;
#else
constexpr auto kReleasePlatform = CvReleasePlatform::Unsupported;
#endif
}

CvUpdateChecker &CvUpdateChecker::instance()
{
    static CvUpdateChecker s_instance;
    return s_instance;
}

CvUpdateChecker::CvUpdateChecker(QObject *parent) : QObject(parent) {}

void CvUpdateChecker::check_once()
{
    if (m_started || kReleasePlatform == CvReleasePlatform::Unsupported)
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
    if (parse_error.error != QJsonParseError::NoError || !doc.isArray())
        return;

    std::string newest_version = OBS_ZOOM_PLUGIN_VERSION;
    for (const auto &entry : doc.array()) {
        const QJsonObject release = entry.toObject();
        if (release.value(QStringLiteral("draft")).toBool(true) ||
            release.value(QStringLiteral("prerelease")).toBool(true))
            continue;

        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        const std::string version = tag.toStdString();
        const QString html_url = release.value(QStringLiteral("html_url")).toString();
        if (html_url.isEmpty() || !cv_is_newer_version(newest_version, version))
            continue;

        for (const auto &entry_asset : release.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject asset = entry_asset.toObject();
            if (asset.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded") ||
                asset.value(QStringLiteral("browser_download_url")).toString().isEmpty() ||
                !cv_is_release_package(version, asset.value(QStringLiteral("name")).toString().toStdString(),
                                       kReleasePlatform))
                continue;

            newest_version = version;
            m_update_tag = tag;
            m_update_url = html_url;
            break;
        }
    }

    if (!m_update_tag.isEmpty()) {
        m_has_update = true;
        emit update_available(m_update_tag, m_update_url);
    }
}
