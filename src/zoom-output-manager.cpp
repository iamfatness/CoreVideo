#include "zoom-output-manager.h"
#include "zoom-source.h"
#include <algorithm>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <util/config-file.h>

static constexpr const char *PRESET_SECTION = "ZoomPlugin";
static constexpr const char *PRESET_KEY = "OutputPresets";

static QString audio_mode_to_string(AudioChannelMode mode)
{
    return mode == AudioChannelMode::Stereo ? "stereo" : "mono";
}

static AudioChannelMode audio_mode_from_string(const QString &mode)
{
    return mode == "stereo" ? AudioChannelMode::Stereo : AudioChannelMode::Mono;
}

ZoomOutputManager &ZoomOutputManager::instance()
{
    static ZoomOutputManager inst;
    return inst;
}

void ZoomOutputManager::register_source(ZoomSource *source)
{
    if (!source) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (std::find(m_sources.begin(), m_sources.end(), source) == m_sources.end())
        m_sources.push_back(source);
}

void ZoomOutputManager::unregister_source(ZoomSource *source)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sources.erase(std::remove(m_sources.begin(), m_sources.end(), source),
                    m_sources.end());
}

std::vector<ZoomOutputInfo> ZoomOutputManager::outputs() const
{
    std::vector<ZoomSource *> sources;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        sources = m_sources;
    }

    std::vector<ZoomOutputInfo> out;
    out.reserve(sources.size());
    for (auto *source : sources) {
        if (!source) continue;
        out.push_back(source->output_info());
    }
    return out;
}

bool ZoomOutputManager::configure_output(const std::string &source_name,
                                         uint32_t participant_id,
                                         bool active_speaker,
                                         bool isolate_audio,
                                         AudioChannelMode audio_mode)
{
    std::vector<ZoomSource *> sources;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        sources = m_sources;
    }

    for (auto *source : sources) {
        if (!source) continue;
        if (source->output_name() != source_name) continue;
        source->configure_output(participant_id, active_speaker,
                                 isolate_audio, audio_mode);
        return true;
    }
    return false;
}

std::vector<ZoomOutputPreset> ZoomOutputManager::load_presets() const
{
    config_t *cfg = obs_frontend_get_global_config();
    const char *raw = config_get_string(cfg, PRESET_SECTION, PRESET_KEY);
    std::vector<ZoomOutputPreset> presets;
    if (!raw || !*raw) return presets;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return presets;

    for (const auto preset_value : doc.object().value("presets").toArray()) {
        const QJsonObject preset_obj = preset_value.toObject();
        ZoomOutputPreset preset;
        preset.name = preset_obj.value("name").toString().toStdString();
        if (preset.name.empty()) continue;

        for (const auto output_value : preset_obj.value("outputs").toArray()) {
            const QJsonObject output_obj = output_value.toObject();
            ZoomOutputInfo info;
            info.source_name = output_obj.value("source").toString().toStdString();
            info.participant_id = static_cast<uint32_t>(
                output_obj.value("participant_id").toInt(0));
            info.active_speaker = output_obj.value("active_speaker").toBool(false);
            info.isolate_audio = output_obj.value("isolate_audio").toBool(false);
            info.audio_mode = audio_mode_from_string(
                output_obj.value("audio_channels").toString("mono"));
            if (!info.source_name.empty())
                preset.outputs.push_back(info);
        }
        presets.push_back(preset);
    }
    return presets;
}

void ZoomOutputManager::save_presets(
    const std::vector<ZoomOutputPreset> &presets) const
{
    QJsonArray preset_array;
    for (const auto &preset : presets) {
        QJsonObject preset_obj;
        preset_obj["name"] = QString::fromStdString(preset.name);

        QJsonArray output_array;
        for (const auto &output : preset.outputs) {
            QJsonObject output_obj;
            output_obj["source"] = QString::fromStdString(output.source_name);
            output_obj["participant_id"] = static_cast<int>(output.participant_id);
            output_obj["active_speaker"] = output.active_speaker;
            output_obj["isolate_audio"] = output.isolate_audio;
            output_obj["audio_channels"] = audio_mode_to_string(output.audio_mode);
            output_array.append(output_obj);
        }
        preset_obj["outputs"] = output_array;
        preset_array.append(preset_obj);
    }

    QJsonObject root;
    root["presets"] = preset_array;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    config_t *cfg = obs_frontend_get_global_config();
    config_set_string(cfg, PRESET_SECTION, PRESET_KEY, json.constData());
    config_save_safe(cfg, "tmp", nullptr);
}

bool ZoomOutputManager::save_preset(const std::string &name)
{
    if (name.empty()) return false;
    auto presets = load_presets();
    ZoomOutputPreset preset;
    preset.name = name;
    preset.outputs = outputs();

    auto it = std::find_if(presets.begin(), presets.end(),
        [&](const ZoomOutputPreset &p) { return p.name == name; });
    if (it != presets.end())
        *it = preset;
    else
        presets.push_back(preset);

    save_presets(presets);
    return true;
}

bool ZoomOutputManager::apply_preset(const std::string &name)
{
    const auto presets = load_presets();
    auto it = std::find_if(presets.begin(), presets.end(),
        [&](const ZoomOutputPreset &p) { return p.name == name; });
    if (it == presets.end()) return false;

    bool applied_any = false;
    for (const auto &output : it->outputs) {
        applied_any |= configure_output(output.source_name, output.participant_id,
                                        output.active_speaker, output.isolate_audio,
                                        output.audio_mode);
    }
    return applied_any;
}

bool ZoomOutputManager::delete_preset(const std::string &name)
{
    auto presets = load_presets();
    const auto old_size = presets.size();
    presets.erase(std::remove_if(presets.begin(), presets.end(),
        [&](const ZoomOutputPreset &p) { return p.name == name; }),
        presets.end());
    if (presets.size() == old_size) return false;
    save_presets(presets);
    return true;
}

std::vector<std::string> ZoomOutputManager::preset_names() const
{
    std::vector<std::string> names;
    for (const auto &preset : load_presets())
        names.push_back(preset.name);
    return names;
}
