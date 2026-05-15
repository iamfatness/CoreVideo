#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "../../src/engine-ipc.h"
#if __has_include(<zoom_sdk_raw_data_def.h>)
#include <zoom_sdk_raw_data_def.h>
#else
#include <rawdata_def.h>
#endif
#if __has_include(<rawdata/rawdata_audio_helper_interface.h>)
#include <rawdata/rawdata_audio_helper_interface.h>
#else
#include <rawdata_audio_helper_interface.h>
#endif

class EngineAudio : public ZOOMSDK::IZoomSDKAudioRawDataDelegate {
public:
    static EngineAudio &instance();

    bool init(IpcFd e2p_fd, const std::string &source_uuid);
    bool retry_subscribe(const std::string &reason);
    void remove(const std::string &source_uuid);
    void shutdown();

    // IZoomSDKAudioRawDataDelegate
    void onMixedAudioRawDataReceived(AudioRawData *data) override;
    void onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onShareAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onOneWayInterpreterAudioRawDataReceived(AudioRawData *data,
                                                 const zchar_t *pLanguageName) override;

private:
    EngineAudio() = default;
    bool subscribe_if_needed(const std::string &source_uuid,
                             const std::string &stage);
    struct AudioTarget {
        explicit AudioTarget(IpcFd e2p) : e2p_fd(e2p) {}
        IpcFd e2p_fd;
        ShmRegion shm;
        uint64_t frame_count = 0;
    };

    bool ensure_shm(AudioTarget &target,
                    const std::string &source_uuid,
                    uint32_t byte_len);

    IpcFd       m_e2p_fd     = kIpcInvalidFd;
    std::mutex  m_subscribe_mtx;
    std::mutex  m_targets_mtx;
    std::unordered_map<std::string, std::unique_ptr<AudioTarget>> m_targets;
    bool        m_subscribed = false;
};
