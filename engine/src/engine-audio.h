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

    // Audio routing per source:
    //   isolate_audio=true             → only participant_id's one-way audio
    //   audience_audio=true            → one-way audio of every participant NOT
    //                                    covered by any isolate target (the
    //                                    "residual active speaker"). Useful for
    //                                    overflow mics when iso channels are
    //                                    bound to named talent.
    //   neither (default)              → the full meeting mix
    // Both true is treated as isolate (isolate wins).
    bool init(IpcFd e2p_fd,
              const std::string &source_uuid,
              uint32_t participant_id,
              bool isolate_audio,
              bool audience_audio);
    bool retry_subscribe(const std::string &reason);
    void reset_subscription(const std::string &reason);
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
        AudioTarget(IpcFd e2p, uint32_t pid, bool isolate, bool audience)
            : e2p_fd(e2p), participant_id(pid),
              isolate_audio(isolate), audience_audio(audience) {}
        IpcFd e2p_fd;
        uint32_t participant_id = 0;
        bool isolate_audio = false;
        bool audience_audio = false;
        ShmRegion shm;
        uint64_t frame_count = 0;
    };

    bool ensure_shm(AudioTarget &target,
                    const std::string &source_uuid,
                    uint32_t byte_len);
    void output_audio_frame(AudioTarget &target,
                            const std::string &source_uuid,
                            AudioRawData *data,
                            const char *stage);

    IpcFd       m_e2p_fd     = kIpcInvalidFd;
    std::mutex  m_subscribe_mtx;
    std::mutex  m_targets_mtx;
    std::unordered_map<std::string, std::unique_ptr<AudioTarget>> m_targets;
    bool        m_subscribed = false;
};
