#pragma once

#include <cstdint>

struct DirectorAutomationSettings {
    uint32_t activeSpeakerSensitivityMs = 300;
    uint32_t activeSpeakerHoldMs = 2000;
    bool manualSupersedeEnabled = true;
    bool autoTakeOnSpeakerChange = false;
};

struct DirectorAutomationState {
    int manualParticipantId = -1;
    int lastAutoSpeakerId = -1;
    uint64_t lastSwitchMs = 0;
};
