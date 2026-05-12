#pragma once
#include <cstdint>
#include <string>
#include <windows.h>
#include "../../third_party/zoom-sdk/h/rawdata_def.h"
#include "../../third_party/zoom-sdk/h/rawdata_audio_helper_interface.h"

#pragma pack(push, 1)
struct ShmAudioHeader { uint32_t sample_rate; uint16_t channels; uint32_t byte_len; };
#pragma pack(pop)

class EngineAudio : public ZOOMSDK::IZoomSDKAudioRawDataDelegate {
public:
    static EngineAudio &instance();

    bool init(HANDLE e2p_pipe, const std::string &source_uuid);
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

    HANDLE      m_e2p_pipe   = nullptr;
    std::string m_source_uuid;
    HANDLE      m_shm_handle = nullptr;
    void       *m_shm_ptr    = nullptr;
    size_t      m_shm_size   = 0;
    bool        m_subscribed = false;
};
