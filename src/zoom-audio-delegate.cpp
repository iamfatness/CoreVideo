#include "zoom-audio-delegate.h"
#include "zoom-audio-router.h"
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
    if (m_registered) return true;

    ZoomAudioRouter &router = ZoomAudioRouter::instance();
    router.add_mixed_sink(this,
        [this](AudioRawData *d) { on_mixed_audio(d); });
    router.add_one_way_sink(this,
        [this](AudioRawData *d, uint32_t uid) { on_one_way_audio(d, uid); });

    m_registered = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Audio delegate registered with router");
    return true;
}

void ZoomAudioDelegate::unsubscribe()
{
    if (!m_registered) return;
    ZoomAudioRouter::instance().remove_mixed_sink(this);
    ZoomAudioRouter::instance().remove_one_way_sink(this);
    m_registered = false;
    m_last_audio_ns.store(0, std::memory_order_relaxed);
    m_last_sample_rate.store(0, std::memory_order_relaxed);
    m_last_channels.store(0, std::memory_order_relaxed);
    m_last_byte_len.store(0, std::memory_order_relaxed);
}

void ZoomAudioDelegate::set_channel_mode(AudioChannelMode mode)
{
    m_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

AudioChannelMode ZoomAudioDelegate::channel_mode() const
{
    return static_cast<AudioChannelMode>(m_mode.load(std::memory_order_relaxed));
}

void ZoomAudioDelegate::set_isolated_user(uint32_t user_id)
{
    m_isolated_user.store(user_id, std::memory_order_relaxed);
}

uint32_t ZoomAudioDelegate::isolated_user() const
{
    return m_isolated_user.load(std::memory_order_relaxed);
}

// ── Router callbacks ──────────────────────────────────────────────────────────

void ZoomAudioDelegate::on_mixed_audio(AudioRawData *data)
{
    if (m_isolated_user.load(std::memory_order_relaxed) != 0) return;
    push_audio(data);
}

void ZoomAudioDelegate::on_one_way_audio(AudioRawData *data, uint32_t user_id)
{
    uint32_t target = m_isolated_user.load(std::memory_order_relaxed);
    if (target == 0 || user_id != target) return;
    push_audio(data);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void ZoomAudioDelegate::push_audio(AudioRawData *data)
{
    if (!data || !m_source) return;
    note_audio(data);
    const auto mode = static_cast<AudioChannelMode>(
        m_mode.load(std::memory_order_relaxed));
    if (mode == AudioChannelMode::Stereo)
        push_stereo(data);
    else
        push_mono(data);
}

void ZoomAudioDelegate::note_audio(AudioRawData *data)
{
    if (!data) return;
    m_audio_count.fetch_add(1, std::memory_order_relaxed);
    m_last_audio_ns.store(os_gettime_ns(), std::memory_order_relaxed);
    m_last_sample_rate.store(data->GetSampleRate(), std::memory_order_relaxed);
    m_last_channels.store(static_cast<uint16_t>(data->GetChannelNum()),
                          std::memory_order_relaxed);
    m_last_byte_len.store(data->GetBufferLen(), std::memory_order_relaxed);
}

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
    const uint32_t byte_len     = data->GetBufferLen();
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
