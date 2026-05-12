#include "zoom-audio-delegate.h"
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <cstring>
static constexpr uint32_t ZOOM_SAMPLE_RATE      = 32000;
static constexpr uint32_t ZOOM_BYTES_PER_SAMPLE = sizeof(int16_t);
ZoomAudioDelegate::ZoomAudioDelegate(obs_source_t *source, AudioChannelMode mode)
    : m_source(source), m_mode(static_cast<int>(mode)) {}
void ZoomAudioDelegate::set_channel_mode(AudioChannelMode mode)
{ m_mode.store(static_cast<int>(mode), std::memory_order_relaxed); }
AudioChannelMode ZoomAudioDelegate::channel_mode() const
{ return static_cast<AudioChannelMode>(m_mode.load(std::memory_order_relaxed)); }
void ZoomAudioDelegate::onMixedAudioRawDataReceived(ZOOM_SDK_NAMESPACE::AudioRawData *data)
{
    if (!data || !m_source) return;
    if (static_cast<AudioChannelMode>(m_mode.load(std::memory_order_relaxed)) == AudioChannelMode::Stereo)
        push_stereo(data);
    else
        push_mono(data);
}
void ZoomAudioDelegate::onOneWayAudioRawDataReceived(ZOOM_SDK_NAMESPACE::AudioRawData *, uint32_t) {}
void ZoomAudioDelegate::push_mono(ZOOM_SDK_NAMESPACE::AudioRawData *data)
{
    const uint32_t byte_len = data->GetBufferLen();
    if (byte_len == 0) return;
    obs_source_audio audio = {};
    audio.data[0]          = reinterpret_cast<const uint8_t *>(data->GetBuffer());
    audio.frames           = byte_len / ZOOM_BYTES_PER_SAMPLE;
    audio.format           = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec  = ZOOM_SAMPLE_RATE;
    audio.speakers         = SPEAKERS_MONO;
    audio.timestamp        = os_gettime_ns();
    obs_source_output_audio(m_source, &audio);
}
void ZoomAudioDelegate::push_stereo(ZOOM_SDK_NAMESPACE::AudioRawData *data)
{
    const uint32_t byte_len     = data->GetBufferLen();
    if (byte_len == 0) return;
    const uint32_t mono_frames  = byte_len / ZOOM_BYTES_PER_SAMPLE;
    const uint32_t stereo_count = mono_frames * 2;
    if (m_stereo_buf.size() < stereo_count) m_stereo_buf.resize(stereo_count);
    const int16_t *src = reinterpret_cast<const int16_t *>(data->GetBuffer());
    int16_t       *dst = m_stereo_buf.data();
    for (uint32_t i = 0; i < mono_frames; ++i) { dst[i*2] = src[i]; dst[i*2+1] = src[i]; }
    obs_source_audio audio = {};
    audio.data[0]          = reinterpret_cast<const uint8_t *>(dst);
    audio.frames           = mono_frames;
    audio.format           = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec  = ZOOM_SAMPLE_RATE;
    audio.speakers         = SPEAKERS_STEREO;
    audio.timestamp        = os_gettime_ns();
    obs_source_output_audio(m_source, &audio);
}
