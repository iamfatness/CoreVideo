#include "zoom-audio-delegate.h"
#include "../third_party/zoom-sdk/h/zoom_rawdata_api.h"
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <cstring>

static constexpr uint32_t ZOOM_BYTES_PER_SAMPLE = sizeof(int16_t);

ZoomAudioDelegate::ZoomAudioDelegate(obs_source_t *source, AudioChannelMode mode)
    : m_source(source), m_mode(static_cast<int>(mode)) {}

ZoomAudioDelegate::~ZoomAudioDelegate()
{
    unsubscribe();
}

bool ZoomAudioDelegate::subscribe()
{
    if (m_subscribed) return true;
    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (!helper) {
        blog(LOG_ERROR, "[obs-zoom-plugin] GetAudioRawdataHelper returned null");
        return false;
    }
    ZOOMSDK::SDKError err = helper->subscribe(this);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Audio subscribe failed: %d", static_cast<int>(err));
        return false;
    }
    m_subscribed = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Audio subscribed (48kHz mixed)");
    return true;
}

void ZoomAudioDelegate::unsubscribe()
{
    if (!m_subscribed) return;
    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (helper) helper->unSubscribe();
    m_subscribed = false;
}

void ZoomAudioDelegate::set_channel_mode(AudioChannelMode mode)
{
    m_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

AudioChannelMode ZoomAudioDelegate::channel_mode() const
{
    return static_cast<AudioChannelMode>(m_mode.load(std::memory_order_relaxed));
}

// ── IZoomSDKAudioRawDataDelegate ─────────────────────────────────────────────

void ZoomAudioDelegate::onMixedAudioRawDataReceived(AudioRawData *data)
{
    if (!data || !m_source) return;
    const auto mode = static_cast<AudioChannelMode>(
        m_mode.load(std::memory_order_relaxed));
    if (mode == AudioChannelMode::Stereo)
        push_stereo(data);
    else
        push_mono(data);
}

// Per-participant audio is not used for the mixed broadcast feed
void ZoomAudioDelegate::onOneWayAudioRawDataReceived(AudioRawData *, uint32_t) {}
void ZoomAudioDelegate::onShareAudioRawDataReceived(AudioRawData *, uint32_t) {}
void ZoomAudioDelegate::onOneWayInterpreterAudioRawDataReceived(AudioRawData *,
                                                                 const zchar_t *) {}

// ── Private helpers ───────────────────────────────────────────────────────────

void ZoomAudioDelegate::push_mono(AudioRawData *data)
{
    const uint32_t byte_len = data->GetBufferLen();
    if (byte_len == 0) return;

    obs_source_audio audio = {};
    audio.data[0]         = reinterpret_cast<const uint8_t *>(data->GetBuffer());
    audio.frames          = byte_len / ZOOM_BYTES_PER_SAMPLE;
    audio.format          = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec = data->GetSampleRate();
    audio.speakers        = SPEAKERS_MONO;
    audio.timestamp       = os_gettime_ns();
    obs_source_output_audio(m_source, &audio);
}

void ZoomAudioDelegate::push_stereo(AudioRawData *data)
{
    const uint32_t byte_len    = data->GetBufferLen();
    if (byte_len == 0) return;
    const uint32_t mono_frames  = byte_len / ZOOM_BYTES_PER_SAMPLE;
    const uint32_t stereo_count = mono_frames * 2;

    if (m_stereo_buf.size() < stereo_count)
        m_stereo_buf.resize(stereo_count);

    const int16_t *src = reinterpret_cast<const int16_t *>(data->GetBuffer());
    int16_t       *dst = m_stereo_buf.data();
    for (uint32_t i = 0; i < mono_frames; ++i) {
        dst[i * 2]     = src[i];
        dst[i * 2 + 1] = src[i];
    }

    obs_source_audio audio = {};
    audio.data[0]         = reinterpret_cast<const uint8_t *>(dst);
    audio.frames          = mono_frames;
    audio.format          = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec = data->GetSampleRate();
    audio.speakers        = SPEAKERS_STEREO;
    audio.timestamp       = os_gettime_ns();
    obs_source_output_audio(m_source, &audio);
}
