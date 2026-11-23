#include "engine-input-policy.h"
#include <cstdlib>
#include <iostream>
#include <vector>
struct Device {
    std::wstring id, name;
    bool selected = false;
    const wchar_t *GetDeviceId() { return id.c_str(); }
    const wchar_t *GetDeviceName() { return name.c_str(); }
    bool IsSelectedDevice() { return selected; }
};
struct Devices {
    std::vector<Device> values{{L"stable-a", L"Duplicate"}, {L"stable-b", L"Duplicate"}};
    int GetCount() { return static_cast<int>(values.size()); }
    Device *GetItem(int i) { return &values[i]; }
};
struct Audio {
    Devices devices;
    bool mute = false, auto_adjust = true, inventory = true;
    bool confirm_selection = true;
    int mute_error = 0, adjust_error = 0, selection_error = 0, selections = 0;
    std::wstring selected;
    int EnableAlwaysMuteMicWhenJoinVoip(bool value) { mute = value && !mute_error; return mute_error; }
    bool IsAlwaysMuteMicWhenJoinVoipEnabled() { return mute; }
    int EnableAutoAdjustMic(bool value) { auto_adjust = value || adjust_error; return adjust_error; }
    bool IsAutoAdjustMicEnabled() { return auto_adjust; }
    Devices *GetMicList() { return inventory ? &devices : nullptr; }
    int SelectMic(const wchar_t *id, const wchar_t *) {
        ++selections; selected = id;
        for (auto &device : devices.values) device.selected = confirm_selection && device.id == id;
        return selection_error;
    }
};
void check(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
int main() {
    Audio a;
    check(!prepare_engine_input(&a, "unchanged", L"old-id"), "default failed");
    check(a.mute && !a.auto_adjust && a.selections == 0, "default changed device or failed to mute");
    check(!prepare_engine_input(&a, "device", L"stable-b") && a.selected == L"stable-b", "selected by label/index");
    a.devices.values.pop_back(); // unplug between joins/reconnects
    check(prepare_engine_input(&a, "device", L"stable-b") != nullptr && a.selections == 1, "missing input silently fell back");
    check(prepare_engine_input(&a, "device", L"") != nullptr, "empty explicit input accepted");
    a.inventory = false;
    check(prepare_engine_input(&a, "device", L"stable-a") != nullptr, "missing inventory accepted");
    a.inventory = true; a.selection_error = 1;
    check(prepare_engine_input(&a, "device", L"stable-a") != nullptr, "selection failure ignored");
    a.selection_error = 0; a.confirm_selection = false;
    check(prepare_engine_input(&a, "device", L"stable-a") != nullptr, "SDK fallback ignored");
    a.mute_error = 1;
    check(prepare_engine_input(&a, "unchanged", L"") != nullptr, "mute failure ignored");
    a.mute_error = 0; a.adjust_error = 1;
    check(prepare_engine_input(&a, "unchanged", L"") != nullptr, "auto gain failure ignored");
    check(prepare_engine_input(&a, "typo", L"") != nullptr, "unknown mode silently fell back");
    check(prepare_engine_input<Audio>(nullptr, "unchanged", L"") != nullptr, "null settings accepted");
    a.adjust_error = 0;
    check(!prepare_engine_input(&a, "silent", L""), "explicit silent mode failed mute setup");
}
