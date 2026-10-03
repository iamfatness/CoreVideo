#include "ffmpeg-runtime-installer.h"
#include "ffmpeg-runtime-fs.h"
#include "ffmpeg-runtime-locate.h"
#include "ffmpeg-runtime-plan.h"

#include <obs-module.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QThread>

static const char *kNothingChanged = " Nothing was changed.";

FfmpegRuntimeInstaller &FfmpegRuntimeInstaller::instance()
{
    static FfmpegRuntimeInstaller s;
    return s;
}

void FfmpegRuntimeInstaller::start_download()
{
    if (m_busy) return;  // one download at a time
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) {
        emit finished(false, QStringLiteral(
            "CoreVideo has no FFmpeg download for this platform. Choose an existing ffmpeg instead."));
        return;
    }
    m_root = cv_ffmpeg_install_root();
    if (m_root.isEmpty()) {
        emit finished(false, QStringLiteral("Could not locate the CoreVideo settings folder.") + kNothingChanged);
        return;
    }
    cvff::clean_leftovers(cv_ffmpeg_fs_path(m_root));

    // Archive + extracted exe + staged copy can coexist briefly.
    const QStorageInfo storage(m_root);
    const qint64 needed = qint64(pin->size_bytes) * 3;
    if (storage.isValid() && storage.bytesAvailable() < needed) {
        emit finished(false, QString("Not enough disk space: FFmpeg needs about %1 MB free in %2.")
                                 .arg(needed / (1024 * 1024)).arg(QDir::toNativeSeparators(m_root)) +
                                 kNothingChanged);
        return;
    }

    m_file = new QFile(QDir(m_root).filePath(QStringLiteral("download.part")), this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = m_file->errorString();
        delete m_file; m_file = nullptr;
        emit finished(false, QStringLiteral("Could not write to the CoreVideo settings folder: ") + why + kNothingChanged);
        return;
    }

    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    m_hash.reset();
    m_received = 0;
    m_cancelled = false;
    m_oversized = false;
    m_write_error.clear();
    m_abort = std::make_shared<std::atomic<bool>>(false);
    m_busy = true;
    emit state_changed();

    QNetworkRequest req{QUrl(QString::fromUtf8(pin->url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);  // 60 s without progress -> TimeoutError
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("CoreVideo-OBS-Plugin"));
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &FfmpegRuntimeInstaller::on_ready_read);
    connect(m_reply, &QNetworkReply::finished, this, &FfmpegRuntimeInstaller::on_download_finished);
    blog(LOG_INFO, "[obs-zoom-plugin] FFmpeg download started: %s", pin->url);
}

void FfmpegRuntimeInstaller::on_ready_read()
{
    if (!m_reply || !m_file) return;
    const QByteArray chunk = m_reply->readAll();
    m_received += chunk.size();
    m_hash.addData(chunk);
    // The pin must vouch for the bytes actually extracted, so a short write
    // is a failure here, not a silent divergence from what was hashed.
    if (m_file->write(chunk) != chunk.size()) {
        if (m_write_error.isEmpty()) m_write_error = m_file->errorString();
        m_reply->abort();
        return;
    }
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!ffmpeg_download_size_ok(quint64(m_received), pin->size_bytes)) {
        m_oversized = true;
        m_reply->abort();
        return;
    }
    emit progress(m_received, qint64(pin->size_bytes));
}

void FfmpegRuntimeInstaller::cancel()
{
    if (!m_busy) return;
    m_cancelled = true;
    if (m_abort && m_worker && m_worker->isRunning()) *m_abort = true;  // worker phase
    if (m_reply) m_reply->abort();
}

void FfmpegRuntimeInstaller::shutdown()
{
    m_shut_down = true;
    if (m_abort) *m_abort = true;
    if (m_reply) m_reply->abort();
    if (m_worker && m_worker->isRunning()) m_worker->wait(10000);
}

void FfmpegRuntimeInstaller::on_download_finished()
{
    QNetworkReply *reply = m_reply;
    on_ready_read();  // drain anything left
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    const auto err = reply->error();
    const QString err_text = reply->errorString();
    reply->deleteLater();
    m_reply = nullptr;
    const bool flushed = m_file->flush();
    m_file->close();
    const bool file_ok = flushed && m_file->error() == QFileDevice::NoError;
    const QString flush_error = m_file->errorString();
    const QString archive = m_file->fileName();
    delete m_file; m_file = nullptr;

    if (m_shut_down) return;
    if (!m_write_error.isEmpty()) {
        finish(false, QStringLiteral("Could not save the download: ") + m_write_error + "." + kNothingChanged);
        return;
    }

    if (m_cancelled) { finish(false, QStringLiteral("Download cancelled.") + kNothingChanged); return; }
    if (m_oversized) { finish(false, QStringLiteral("Download failed: the file was larger than expected.") + kNothingChanged); return; }
    if (err != QNetworkReply::NoError) {
        const bool unreachable = err == QNetworkReply::HostNotFoundError ||
            err == QNetworkReply::ConnectionRefusedError || err == QNetworkReply::TimeoutError ||
            err == QNetworkReply::TemporaryNetworkFailureError ||
            err == QNetworkReply::NetworkSessionFailedError;
        finish(false, unreachable
            ? QString("Download failed: could not reach %1. Check your internet connection.").arg(pin->host) + kNothingChanged
            : QStringLiteral("Download failed: ") + err_text + "." + kNothingChanged);
        return;
    }
    if (!file_ok) {
        finish(false, QStringLiteral("Could not save the download: ") + flush_error + "." + kNothingChanged);
        return;
    }
    if (quint64(m_received) != pin->size_bytes ||
        m_hash.result().toHex() != QByteArray(pin->sha256)) {
        finish(false, QStringLiteral("Download failed: file didn't match the expected checksum.") + kNothingChanged);
        return;
    }

    // Extraction and the test-run block for seconds; keep them off the UI thread.
    const QString root = m_root;
    const auto abort = m_abort;  // the worker gets a copy, never this's state
    QThread *worker = QThread::create([this, root, archive, abort] {
        const QString failure = install_from_archive(root, archive, abort);
        QMetaObject::invokeMethod(this, [this, failure] {
            finish(failure.isEmpty(), failure.isEmpty()
                ? QStringLiteral("FFmpeg %1 installed.").arg(ffmpeg_runtime_pin_for_host()->version)
                : failure);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_worker = worker;
    worker->start();
}

static bool run_tool(const QString &program, const QStringList &args, QByteArray *out, int timeout_ms,
                     const std::atomic<bool> &abort)
{
    // Argument LIST, never a joined string: profile paths contain spaces and
    // non-ASCII characters (REVIEW FOCUS 3).
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForStarted(2000)) {
        blog(LOG_WARNING, "[obs-zoom-plugin] FFmpeg install: could not start %s: %s",
             program.toUtf8().constData(), p.errorString().toUtf8().constData());
        return false;
    }
    QElapsedTimer t;
    t.start();
    while (!p.waitForFinished(200)) {
        if (abort.load() || t.elapsed() >= timeout_ms) {
            p.kill();
            p.waitForFinished(2000);
            return false;
        }
    }
    if (out) *out = p.readAllStandardOutput();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

QString FfmpegRuntimeInstaller::install_from_archive(const QString &root, const QString &archive,
                                                 const std::shared_ptr<std::atomic<bool>> &abort_ptr)
{
    const std::atomic<bool> &abort = *abort_ptr;
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    const QDir dir(root);
    const QString extract = dir.filePath(QStringLiteral("extract"));
    const QString staging = dir.filePath(QStringLiteral("staging"));
    QDir().mkpath(extract);
    QDir().mkpath(staging);
    const bool was_complete = cvff::install_complete(cv_ffmpeg_fs_path(root), pin->exe_name);
    auto fail = [&](const QString &why) {
        cvff::clean_leftovers(cv_ffmpeg_fs_path(root));
        // "Nothing was changed" is only true if the previous install survived.
        const bool now_complete = cvff::install_complete(cv_ffmpeg_fs_path(root), pin->exe_name);
        return why + (now_complete == was_complete
            ? QString(kNothingChanged)
            : QStringLiteral(" Your previous FFmpeg could not be restored; use Download FFmpeg again."));
    };
    auto cancelled = [&] { return fail(QStringLiteral("Download cancelled.")); };
    // A run_tool failure caused by cancel() is a cancellation, not that step's fault.
    auto step_failed = [&](const QString &why) { return abort.load() ? cancelled() : fail(why); };

    // The pin vouches for the bytes we EXTRACT, not just the bytes we hashed
    // while streaming: re-hash the closed file from disk and trust only that.
    {
        QFile f(archive);
        QCryptographicHash h(QCryptographicHash::Sha256);
        qint64 total = 0;
        bool read_ok = f.open(QIODevice::ReadOnly);
        while (read_ok && !f.atEnd()) {
            if (abort.load()) return cancelled();
            const QByteArray buf = f.read(1 << 20);
            if (buf.isEmpty() && !f.atEnd()) { read_ok = false; break; }
            total += buf.size();
            h.addData(buf);
        }
        if (!read_ok || quint64(total) != pin->size_bytes ||
            h.result().toHex() != QByteArray(pin->sha256))
            return fail(QStringLiteral("Download failed: file didn't match the expected checksum."));
    }

#if defined(_WIN32)
    const QString tar = QDir(QProcessEnvironment::systemEnvironment().value(
                                 QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows")))
                            .filePath(QStringLiteral("System32/tar.exe"));
    if (!QFileInfo::exists(tar))
        return fail(QStringLiteral("This version of Windows has no built-in tar.exe (Windows 10 1803 or newer). "
                                   "Choose an existing ffmpeg instead."));
    QByteArray listing;
    if (!run_tool(tar, {QStringLiteral("-tf"), archive}, &listing, 60000, abort))
        return step_failed(QStringLiteral("Could not read the downloaded archive."));
#else
    QByteArray listing;
    if (!run_tool(QStringLiteral("/usr/bin/zipinfo"), {QStringLiteral("-1"), archive}, &listing, 60000, abort))
        return step_failed(QStringLiteral("Could not read the downloaded archive."));
#endif
    if (abort.load()) return cancelled();
    bool has_exe = false;
    for (const QByteArray &line : listing.split('\n')) {
        const std::string entry = QString::fromUtf8(line).trimmed().toStdString();
        if (entry.empty()) continue;
        if (!ffmpeg_archive_entry_safe(entry))
            return fail(QStringLiteral("The downloaded archive contains an unsafe path."));
        if (entry == pin->archive_exe) has_exe = true;
    }
    if (!has_exe)
        return fail(QStringLiteral("The downloaded archive does not contain ffmpeg."));

#if defined(_WIN32)
    if (!run_tool(tar, {QStringLiteral("-xf"), archive, QStringLiteral("-C"), extract,
                        QString::fromUtf8(pin->archive_exe)}, nullptr, 300000, abort))
        return step_failed(QStringLiteral("Could not unpack FFmpeg."));
#else
    if (!run_tool(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-x"), QStringLiteral("-k"), archive, extract},
                  nullptr, 300000, abort))
        return step_failed(QStringLiteral("Could not unpack FFmpeg."));
#endif

    if (abort.load()) return cancelled();
    const QString exe = QDir(staging).filePath(QString::fromUtf8(pin->exe_name));
    if (!QFile::copy(QDir(extract).filePath(QString::fromUtf8(pin->archive_exe)), exe))
        return fail(QStringLiteral("Could not stage FFmpeg."));
    QFile::setPermissions(exe, QFile::permissions(exe) | QFileDevice::ExeOwner |
                                   QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);

    char *license = obs_module_file(kFfmpegLicenseDataFile);
    const bool license_ok = license &&
        QFile::copy(QString::fromUtf8(license), QDir(staging).filePath(QStringLiteral("LICENSE.txt")));
    bfree(license);
    if (!license_ok)
        return fail(QStringLiteral("CoreVideo's FFmpeg license file is missing; reinstall CoreVideo."));

    if (abort.load()) return cancelled();
    QByteArray version;
    if (!run_tool(exe, {QStringLiteral("-hide_banner"), QStringLiteral("-version")}, &version, 30000, abort) ||
        !version.startsWith("ffmpeg version"))
        return step_failed(QStringLiteral("The downloaded FFmpeg did not start on this computer."));

    QFile prov(QDir(staging).filePath(QStringLiteral("provenance.txt")));
    if (!prov.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write provenance.txt."));
    prov.write(QByteArray::fromStdString(ffmpeg_provenance_text(
        *pin, QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString())));
    prov.close();

    if (abort.load()) return cancelled();
    QDir(extract).removeRecursively();
    QFile::remove(archive);
    std::string swap_error;
    if (!cvff::swap_in_staging(cv_ffmpeg_fs_path(root), &swap_error))
        return fail(QString::fromStdString(swap_error));
    return QString();
}

void FfmpegRuntimeInstaller::finish(bool ok, const QString &message)
{
    if (m_shut_down) return;
    if (!ok && !m_root.isEmpty())
        cvff::clean_leftovers(cv_ffmpeg_fs_path(m_root));
    m_busy = false;
    blog(ok ? LOG_INFO : LOG_WARNING, "[obs-zoom-plugin] FFmpeg download: %s",
         message.toUtf8().constData());
    emit state_changed();
    emit finished(ok, message);
}

bool FfmpegRuntimeInstaller::remove(bool in_use, QString *error)
{
    if (m_busy) {
        if (error) *error = QStringLiteral("FFmpeg is still downloading.");
        return false;
    }
    std::string why;
    const bool ok = cvff::remove_install(cv_ffmpeg_fs_path(cv_ffmpeg_install_root()), in_use, &why);
    if (!ok && error) *error = QString::fromStdString(why);
    emit state_changed();
    return ok;
}
