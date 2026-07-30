#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// Privacy-respecting, opt-out-able update check against the public GitHub
// Releases API. Design constraints (see docs/policies/privacy-policy.md):
//   - Exactly one plain GET per OBS session, no query params, no identifiers.
//   - Never blocks the caller: the request is fully asynchronous.
//   - Never auto-downloads anything; at most emits update_available() so the
//     dock can show a dismissible CvBanner linking to the releases page.
//   - Fails totally silently on any network/HTTP/parse error - no blog(),
//     no dialog, nothing an offline operator would ever see.
//   - Respects ZoomPluginSettings::check_for_updates_on_startup; callers are
//     responsible for checking that flag before calling check_once().
//
// Process-wide singleton (Meyers pattern, matching ZoomReconnectManager /
// ZoomOutputManager / SpeakerDirector in this codebase) so the "once per OBS
// session" guarantee holds even if the Zoom Control dock is closed and
// reopened without restarting OBS, and so a dock opened after the response
// already arrived can still show the banner immediately.
class CvUpdateChecker : public QObject {
    Q_OBJECT
public:
    static CvUpdateChecker &instance();

    // Issues the async GET the first time it's called in this process; a
    // no-op on every later call (whether or not the first check found an
    // update, is still in flight, or failed).
    void check_once();

    // True once a strictly newer release has been confirmed. Lets a dock
    // created after the response already arrived show the banner right
    // away instead of missing the one-shot signal below.
    bool has_known_update() const { return m_has_update; }
    QString known_update_tag() const { return m_update_tag; }
    QString known_update_url() const { return m_update_url; }

signals:
    // Emitted at most once per process, only when a strictly newer release
    // is found. `tag` is the GitHub release tag (e.g. "v0.1.28"); `html_url`
    // is the release page to link to.
    void update_available(const QString &tag, const QString &html_url);

private:
    explicit CvUpdateChecker(QObject *parent = nullptr);

    void on_reply_finished();

    QNetworkAccessManager *m_manager = nullptr;
    bool m_started = false;
    bool m_has_update = false;
    QString m_update_tag;
    QString m_update_url;
};
