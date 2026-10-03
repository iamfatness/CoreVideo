#pragma once
// Fetches the pinned FFmpeg into the operator's OBS profile when they press
// Download (spec 2026-10-03). Process-wide singleton (Meyers, like
// CvUpdateChecker) so the dock can be closed and reopened mid-download.
// Never runs without an explicit operator click.
#include <QObject>
#include <QCryptographicHash>
#include <QPointer>

#include <atomic>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QThread;

class FfmpegRuntimeInstaller : public QObject {
    Q_OBJECT
public:
    static FfmpegRuntimeInstaller &instance();

    bool busy() const { return m_busy; }
    // Starts a download unless one is already running (a double click must
    // not start two). Emits finished() on every exit.
    void start_download();
    void cancel();
    // Refuses while the managed ffmpeg is recording (a running recording holds the exe open).
    bool remove(bool in_use, QString *error);
    // Called from obs_module_unload(): stops the download, tells the worker to
    // abort, and waits (bounded) for it so no thread outlives the plugin.
    // After this, any queued finish() is a no-op.
    void shutdown();

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString &message);
    void state_changed();

private:
    FfmpegRuntimeInstaller() = default;
    void on_ready_read();
    void on_download_finished();
    void finish(bool ok, const QString &message);
    // Runs on a worker thread: list, extract, copy, test-run, provenance,
    // swap. Returns "" on success, else the operator-facing failure line.
    static QString install_from_archive(const QString &root, const QString &archive,
                                        const std::shared_ptr<std::atomic<bool>> &abort);

    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_received = 0;
    bool m_busy = false;
    bool m_cancelled = false;
    bool m_oversized = false;
    QString m_root;
    QString m_write_error;
    bool m_shut_down = false;
    QPointer<QThread> m_worker;
    std::shared_ptr<std::atomic<bool>> m_abort;
};
