#include "zoom-output-profile.h"
#include <util/platform.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <obs-module.h>

static QString profiles_dir()
{
    char path[512];
    if (os_get_config_path(path, sizeof(path),
            "obs-studio/plugin_config/obs-zoom-plugin/profiles") < 0)
        return {};
    QDir().mkpath(path);
    return QString::fromUtf8(path);
}

static QString profile_path(const std::string &name)
{
    const QString dir = profiles_dir();
    if (dir.isEmpty()) return {};
    return dir + "/" + QString::fromStdString(name) + ".json";
}

namespace ZoomOutputProfile {

std::vector<std::string> list()
{
    const QString dir = profiles_dir();
    if (dir.isEmpty()) return {};
    std::vector<std::string> names;
    for (const QString &entry : QDir(dir).entryList({"*.json"}, QDir::Files, QDir::Name)) {
        const QString name = entry.chopped(5); // strip ".json"
        names.push_back(name.toStdString());
    }
    return names;
}

bool save(const std::string &name, const std::vector<ZoomOutputInfo> &outputs)
{
    const QString path = profile_path(name);
    if (path.isEmpty()) return false;

    QJsonArray arr;
    for (const auto &o : outputs) {
        QJsonObject obj;
        obj["source"]         = QString::fromStdString(o.source_name);
        obj["display_name"]   = QString::fromStdString(o.display_name);
        obj["participant_id"] = static_cast<int>(o.participant_id);
        obj["active_speaker"] = o.active_speaker;
        obj["isolate_audio"]  = o.isolate_audio;
        obj["audio_channels"] = o.audio_mode == AudioChannelMode::Stereo
                                ? "stereo" : "mono";
        arr.append(obj);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    blog(LOG_INFO, "[obs-zoom-plugin] Saved output profile '%s'", name.c_str());
    return true;
}

std::vector<ZoomOutputInfo> load(const std::string &name)
{
    const QString path = profile_path(name);
    if (path.isEmpty()) return {};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return {};

    std::vector<ZoomOutputInfo> outputs;
    for (const QJsonValue &val : doc.array()) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        ZoomOutputInfo o;
        o.source_name    = obj.value("source").toString().toStdString();
        o.display_name   = obj.value("display_name").toString().toStdString();
        o.participant_id = static_cast<uint32_t>(obj.value("participant_id").toInt(0));
        o.active_speaker = obj.value("active_speaker").toBool(false);
        o.isolate_audio  = obj.value("isolate_audio").toBool(false);
        o.audio_mode     = obj.value("audio_channels").toString() == "stereo"
                           ? AudioChannelMode::Stereo : AudioChannelMode::Mono;
        outputs.push_back(std::move(o));
    }
    blog(LOG_INFO, "[obs-zoom-plugin] Loaded output profile '%s' (%zu outputs)",
         name.c_str(), outputs.size());
    return outputs;
}

bool remove(const std::string &name)
{
    const QString path = profile_path(name);
    if (path.isEmpty()) return false;
    return QFile::remove(path);
}

} // namespace ZoomOutputProfile
