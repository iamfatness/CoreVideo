#include "ffmpeg-runtime-locate.h"
#include "ffmpeg-runtime-fs.h"
#include "ffmpeg-runtime-pins.h"

#include <obs-module.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

QString cv_ffmpeg_install_root()
{
    char *raw = obs_module_config_path("ffmpeg");
    const QString root = raw ? QString::fromUtf8(raw) : QString();
    bfree(raw);
    if (!root.isEmpty())
        QDir().mkpath(root);
    return root;
}

QString cv_ffmpeg_managed_exe()
{
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) return QString();
    return QDir(cv_ffmpeg_install_root()).filePath(
        QStringLiteral("current/") + QString::fromUtf8(pin->exe_name));
}

std::filesystem::path cv_ffmpeg_fs_path(const QString &p)
{
#if defined(_WIN32)
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::u8path(p.toUtf8().constData());
#endif
}

bool cv_ffmpeg_managed_installed()
{
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) return false;
    return cvff::install_complete(
        cv_ffmpeg_fs_path(cv_ffmpeg_install_root()), pin->exe_name);
}

FfmpegResolution cv_ffmpeg_resolve(const std::string &configured)
{
    FfmpegResolveInputs in;
    in.configured = configured;
    in.managed_exe = QDir::toNativeSeparators(cv_ffmpeg_managed_exe()).toStdString();
    in.managed_complete = cv_ffmpeg_managed_installed();
#if defined(__APPLE__)
    in.macos = true;
#endif
    in.file_exists = [](const std::string &p) {
        const QFileInfo info(QString::fromStdString(p));
        return info.exists() && info.isFile();
    };
    in.find_on_path = [](const std::string &name) {
        return QStandardPaths::findExecutable(QString::fromStdString(name)).toStdString();
    };
    // The dock may hold either separator style; compare like with like.
    in.configured = QDir::toNativeSeparators(QString::fromStdString(configured).trimmed()).toStdString();
    return ffmpeg_resolve(in);
}
