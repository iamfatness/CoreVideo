// MANUAL, NETWORK: proves the pinned FFmpeg archive for this platform still
// downloads and still hashes to the pin. Run it whenever ffmpeg-runtime-pins.h
// changes and before a release; a drifted pin means every operator's Download
// button fails with a checksum error. Deliberately not in ctest (CI must not
// depend on gyan.dev / martin-riedl.de availability).
#include "ffmpeg-runtime-pins.h"
#include "ffmpeg-runtime-plan.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) {
        std::cout << "no FFmpeg pin for this platform; nothing to check\n";
        return 0;
    }
    std::cout << "downloading " << pin->url << "\n";
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(QString::fromUtf8(pin->url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam.get(req);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 received = 0;
    bool oversized = false;
    QByteArray body;
    QObject::connect(reply, &QNetworkReply::readyRead, [&] {
        const QByteArray chunk = reply->readAll();
        body.append(chunk);
        received += quint64(chunk.size());
        hash.addData(chunk);
        if (!ffmpeg_download_size_ok(received, pin->size_bytes)) {
            oversized = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &app, &QCoreApplication::quit);
    app.exec();
    const QByteArray rest = reply->readAll();
    received += quint64(rest.size());
    hash.addData(rest);
    body.append(rest);

    if (oversized) { std::cerr << "FAIL: archive larger than pin + 10%\n"; return 1; }
    if (reply->error() != QNetworkReply::NoError) {
        std::cerr << "FAIL: " << reply->errorString().toStdString() << "\n";
        return 1;
    }
    const std::string got = hash.result().toHex().toStdString();
    std::cout << "size " << received << " (pin " << pin->size_bytes << ")\n"
              << "sha256 " << got << "\n";
    if (received != pin->size_bytes || got != pin->sha256) {
        std::cerr << "FAIL: archive no longer matches the pin\n";
        return 1;
    }

    // The bytes match; now prove the archive is what the installer expects:
    // every entry passes the safety filter and the pinned exe is inside.
    QTemporaryFile tmp(QDir::temp().filePath(QStringLiteral("cv-ffmpeg-live-XXXXXX.zip")));
    if (!tmp.open() || tmp.write(body) != body.size()) {
        std::cerr << "FAIL: could not save the archive to a temp file\n";
        return 1;
    }
    tmp.close();
    const QString file = tmp.fileName();
#if defined(_WIN32)
    const QString tool = QDir(QProcessEnvironment::systemEnvironment().value(
                                  QStringLiteral("SystemRoot"), QStringLiteral("C:" "\\Windows")))
                             .filePath(QStringLiteral("System32/tar.exe"));
    const QStringList args{QStringLiteral("-tf"), file};
#else
    const QString tool = QStringLiteral("/usr/bin/zipinfo");
    const QStringList args{QStringLiteral("-1"), file};
#endif
    QProcess lister;
    lister.start(tool, args);
    if (!lister.waitForFinished(120000) || lister.exitStatus() != QProcess::NormalExit ||
        lister.exitCode() != 0) {
        std::cerr << "FAIL: could not list the archive with " << tool.toStdString() << "\n";
        return 1;
    }
    bool has_exe = false;
    int entries = 0;
    for (const QByteArray &line : lister.readAllStandardOutput().split('\n')) {
        const std::string entry = QString::fromUtf8(line).trimmed().toStdString();
        if (entry.empty()) continue;
        ++entries;
        if (!ffmpeg_archive_entry_safe(entry)) {
            std::cerr << "FAIL: unsafe archive entry: " << entry << "\n";
            return 1;
        }
        if (entry == pin->archive_exe) has_exe = true;
    }
    if (!has_exe) {
        std::cerr << "FAIL: archive does not contain " << pin->archive_exe << "\n";
        return 1;
    }
    std::cout << "archive listing ok: " << entries << " entries, all safe, contains "
              << pin->archive_exe << "\n";
    std::cout << "ffmpeg runtime live: pin matches upstream\n";
    return 0;
}
