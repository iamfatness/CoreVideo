#pragma once
#include <obs-module.h>
#include <cstdint>
#include <vector>
#include <atomic>
#ifndef ZOOM_SDK_NAMESPACE
namespace ZOOM_SDK_NAMESPACE {
struct AudioRawData {
    virtual char     *GetBuffer()     = 0;
    virtual uint32_t  GetBufferLen()  = 0;
    virtual uint32_t  GetSampleRate() = 0;
    virtual uint32_t  GetChannelNum() = 0;
    virtual ~AudioRawData() = default;
};
struct IZoomSDKAudioRawDataDelegate {
    virtual void onMixedAudioRawDataReceived(AudioRawData *data) = 0;
    virtual void onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t node_id) = 0;
    virtual ~IZoomSDKAudioRawDataDelegate() = default;
};
} // namespace ZOOM_SDK_NAMESPACE
#endif
enum class AudioChannelMode { Mono = 0, Stereo = 1 };
class ZoomAudioDelegate : public ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataDelegate {
public:
    explicit ZoomAudioDelegate(obs_source_t *source, AudioChannelMode mode = AudioChannelMode::Mono);
    ~ZoomAudioDelegate() override = default;
    void set_channel_mode(AudioChannelMode mode);
    AudioChannelMode channel_mode() const;
    void onMixedAudioRawDataReceived(ZOOM_SDK_NAMESPACE::AudioRawData *data) override;
    void onOneWayAudioRawDataReceived(ZOOM_SDK_NAMESPACE::AudioRawData *data, uint32_t node_id) override;
private:
    void push_mono(ZOOM_SDK_NAMESPACE::AudioRawData *data);
    void push_stereo(ZOOM_SDK_NAMESPACE::AudioRawData *data);
    obs_source_t        *m_source;
    std::atomic<int>     m_mode;
    std::vector<int16_t> m_stereo_buf;
};
