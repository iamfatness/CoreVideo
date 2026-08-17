#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "../../src/engine-ipc.h"
#include "../../src/shm-generation.h"
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
    void set_raw_media_active(bool active);
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
        // The generation the CURRENT region was created under; part of the name
        // from gen 2 and published with every audio event. A record of what was
        // published, NOT the counter — the counter survives this struct in the
        // process-wide table (src/shm-generation.h).
        uint32_t shm_gen = 0;
        uint64_t frame_count = 0;
        // True after an ensure_shm() failure has been surfaced as an error —
        // avoids re-emitting once per audio callback while the failure persists.
        bool shm_fail_reported = false;
        // Latch for the byte_len > slot_bytes bounds check in
        // output_audio_frame(). That branch is provably unreachable (see the
        // proof there); the latch exists so that if a future change ever breaks
        // ensure_shm()'s invariant, the engine log says so ONCE rather than 100
        // times a second.
        bool slot_too_small_reported = false;
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
    bool        m_raw_media_active = false;
};
