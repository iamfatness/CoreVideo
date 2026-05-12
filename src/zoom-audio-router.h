#pragma once
#include <functional>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <zoom_sdk_raw_data_def.h>
#include <rawdata/rawdata_audio_helper_interface.h>

// Sole subscriber to IZoomSDKAudioRawDataHelper.
// ZoomAudioDelegate and ZoomParticipantAudioSource register sinks here;
// the router dispatches each incoming frame to all matching sinks.
class ZoomAudioRouter : public ZOOMSDK::IZoomSDKAudioRawDataDelegate {
public:
    static ZoomAudioRouter &instance();

    bool subscribe();
    void unsubscribe();

    // Mixed-feed sink — receives every mixed frame unless overridden by isolation.
    // ZoomAudioDelegate calls this when m_isolated_user == 0.
    using MixedSink  = std::function<void(AudioRawData *)>;
    void add_mixed_sink(void *key, MixedSink cb);
    void remove_mixed_sink(void *key);

    // One-way sink — receives ALL per-user frames with the user_id attached.
    // ZoomAudioDelegate uses this to implement isolation: it filters by m_isolated_user.
    using OneWaySink = std::function<void(AudioRawData *, uint32_t)>;
    void add_one_way_sink(void *key, OneWaySink cb);
    void remove_one_way_sink(void *key);

    // Per-participant sinks — ZoomParticipantAudioSource registers one per user_id.
    using ParticipantSink = std::function<void(AudioRawData *)>;
    void add_participant_sink(uint32_t user_id, void *key, ParticipantSink cb);
    void remove_participant_sink(uint32_t user_id, void *key);

    // Share-audio sink — ZoomShareDelegate registers here to receive computer
    // audio from screen shares (separate stream from participant audio).
    using ShareAudioSink = std::function<void(AudioRawData *, uint32_t)>;
    void add_share_audio_sink(void *key, ShareAudioSink cb);
    void remove_share_audio_sink(void *key);

    // Interpretation-language sink — receives frames for a specific language track.
    // language_name is UTF-8; the sink is responsible for filtering by language.
    using InterpSink = std::function<void(AudioRawData *, const std::string &language_name)>;
    void add_interp_sink(void *key, InterpSink cb);
    void remove_interp_sink(void *key);

    // IZoomSDKAudioRawDataDelegate
    void onMixedAudioRawDataReceived(AudioRawData *data) override;
    void onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onShareAudioRawDataReceived(AudioRawData *data, uint32_t share_user_id) override;
    void onOneWayInterpreterAudioRawDataReceived(AudioRawData *data,
                                                 const zchar_t *language) override;

private:
    ZoomAudioRouter() = default;

    std::mutex  m_mtx;
    std::unordered_map<void *, MixedSink> m_mixed_sinks;
    std::unordered_map<void *, OneWaySink> m_one_way_sinks;
    std::unordered_map<uint32_t, std::unordered_map<void *, ParticipantSink>>
        m_participant_sinks;
    std::unordered_map<void *, ShareAudioSink> m_share_audio_sinks;
    std::unordered_map<void *, InterpSink>     m_interp_sinks;
    bool        m_subscribed = false;
};
