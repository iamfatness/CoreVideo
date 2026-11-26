#pragma once
#include <string>

struct EngineInputDiagnostics {
    int mute_set_code = -1, gain_set_code = -1;
    bool mute_enabled = false, gain_enabled = false;
};

// The default never selects a device. Validate the current SDK inventory on
// every join (including reconnect), using IDs rather than labels or indices.
// Templated so regression tests exercise this exact sequence with a fake SDK.
template<class Audio>
const char *prepare_engine_input(Audio *audio, const std::string &mode,
                                 const std::wstring &id, EngineInputDiagnostics *diagnostics = nullptr)
{
    if (mode != "unchanged" && mode != "device" && mode != "silent")
        return "invalid_input_mode";
    if (!audio) return "audio_settings_unavailable";
    EngineInputDiagnostics local;
    auto &d = diagnostics ? *diagnostics : local;
    d = {};
    d.mute_set_code = static_cast<int>(audio->EnableAlwaysMuteMicWhenJoinVoip(true));
    d.mute_enabled = audio->IsAlwaysMuteMicWhenJoinVoipEnabled();
    if (d.mute_set_code != 0) return "mute_on_join_set_failed";
    if (!d.mute_enabled) return "mute_on_join_unconfirmed";
    d.gain_set_code = static_cast<int>(audio->EnableAutoAdjustMic(false));
    d.gain_enabled = audio->IsAutoAdjustMicEnabled();
    if (d.gain_set_code != 0 || d.gain_enabled)
        return "disable_auto_adjust_failed";
    if (mode != "device") return nullptr;
    if (id.empty()) return "input_device_missing";
    auto *devices = audio->GetMicList();
    if (!devices) return "input_inventory_unavailable";
    for (int i = 0; i < devices->GetCount(); ++i) {
        auto *device = devices->GetItem(i);
        if (!device || !device->GetDeviceId() || id != device->GetDeviceId()) continue;
        if (!device->GetDeviceName()) return "input_device_missing";
        if (audio->SelectMic(device->GetDeviceId(), device->GetDeviceName()) != 0)
            return "input_selection_failed";
        auto *selected = audio->GetMicList();
        if (selected) for (int j = 0; j < selected->GetCount(); ++j) {
            auto *current = selected->GetItem(j);
            if (current && current->GetDeviceId() && id == current->GetDeviceId() &&
                current->IsSelectedDevice()) return nullptr;
        }
        return "input_selection_unconfirmed";
    }
    return "input_device_unavailable";
}
