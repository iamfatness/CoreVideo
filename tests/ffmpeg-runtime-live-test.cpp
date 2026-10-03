// MANUAL, NETWORK: proves the pinned FFmpeg archive for this platform still
// downloads and still hashes to the pin. Run it whenever ffmpeg-runtime-pins.h
// changes and before a release; a drifted pin means every operator's Download
// button fails with a checksum error. Deliberately not in ctest (CI must not
// depend on gyan.dev / martin-riedl.de availability).
#include "ffmpeg-runtime-pins.h"
#include "ffmpeg-runtime-plan.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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
    QObject::connect(reply, &QNetworkReply::readyRead, [&] {
        const QByteArray chunk = reply->readAll();
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
    std::cout << "ffmpeg runtime live: pin matches upstream\n";
    return 0;
}
