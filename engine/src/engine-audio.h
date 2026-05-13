#pragma once
#include <cstdint>
#include <string>
#include "../../src/engine-ipc.h"
#include <rawdata_def.h>
#include <rawdata_audio_helper_interface.h>

class EngineAudio : public ZOOMSDK::IZoomSDKAudioRawDataDelegate {
public:
    static EngineAudio &instance();

    bool init(IpcFd e2p_fd, const std::string &source_uuid);
    void shutdown();

    // IZoomSDKAudioRawDataDelegate
    void onMixedAudioRawDataReceived(AudioRawData *data) override;
    void onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onShareAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onOneWayInterpreterAudioRawDataReceived(AudioRawData *data,
                                                 const zchar_t *pLanguageName) override;

private:
    EngineAudio() = default;
    void ensure_shm(uint32_t byte_len);

    IpcFd       m_e2p_fd     = kIpcInvalidFd;
    std::string m_source_uuid;
    ShmRegion   m_shm;
    bool        m_subscribed = false;
};
