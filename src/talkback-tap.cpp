#include "talkback-tap.h"
#include "talkback-pcm.h"
#include "talkback-ring.h"
#include "shm-generation.h"

#include <obs-module.h>
#include <util/platform.h>

#include <vector>

TalkbackTap::~TalkbackTap() { close(); }

bool TalkbackTap::open(const std::string &source_name, std::string &error_out)
{
    close();
    std::lock_guard<std::mutex> lock(m_mtx);

    obs_source_t *src = obs_get_source_by_name(source_name.c_str());
    if (!src) {
        error_out = "No OBS source named \"" + source_name + "\"";
        return false;
    }

    // OBS's audio format is global, so read it once here rather than
    // per-callback. We pass the rate through instead of resampling: a
    // resampler is a whole subsystem, and OBS runs at 48kHz by default,
    // which the SDK recommends. An unsupported rate is reported loudly --
    // sending at the wrong rate is heard as a chipmunk or a drawl, which an
    // operator would report as "talkback is broken", not "my rate is odd".
    const struct audio_output_info *aoi =
        audio_output_get_info(obs_get_audio());
    if (!aoi) {
        obs_source_release(src);
        error_out = "OBS audio is not running";
        return false;
    }
    const uint32_t rate = aoi->samples_per_sec;
    const uint16_t chans =
        static_cast<uint16_t>(get_audio_channels(aoi->speakers));
    if (!talkback_pcm_rate_supported(rate)) {
        obs_source_release(src);
        error_out = "OBS runs at " + std::to_string(rate) +
                    " Hz, which the Zoom talkback API does not accept. "
                    "Set OBS to 48000 Hz in Settings > Audio.";
        return false;
    }
    if (chans != 1 && chans != 2) {
        obs_source_release(src);
        error_out = "Talkback needs a mono or stereo OBS audio setup; this one "
                    "has " + std::to_string(chans) + " channels.";
        return false;
    }

    // A Windows named section cannot grow while any process maps it, so every
    // region name carries a generation. Talkback never resizes, but it must
    // still not collide with a stale section left by a previous run -- and
    // shm_region_create() reports exactly that case via ShmRegion::last_error
    // / the "opened an existing section" flag documented on the struct.
    m_region_name = shm_next_region(shm_generations(), base_region_name()).name;
    if (!shm_region_create(m_region, m_region_name,
                           shm_audio_region_bytes(kTalkbackSlotBytes))) {
        obs_source_release(src);
        error_out = "Could not create the talkback shared-memory region";
        return false;
    }
    talkback_ring_init(static_cast<ShmAudioHeader *>(m_region.ptr), rate, chans);

    m_source        = src;   // keep the strong ref; released in close()
    m_sample_rate   = rate;
    m_channels      = chans;
    m_last_audio_ms = os_gettime_ns() / 1000000ULL;
    m_open          = true;

    obs_source_add_audio_capture_callback(m_source, audio_cb, this);
    return true;
}

void TalkbackTap::close()
{
    obs_source_t *to_release = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open) return;
        m_open = false;
        to_release = m_source;
        m_source = nullptr;
    }
    // Remove the callback OUTSIDE the lock: libobs takes its own audio mutex
    // here, and the callback takes ours. Holding both in opposite orders on
    // two threads is a classic lock-order inversion.
    if (to_release) {
        obs_source_remove_audio_capture_callback(to_release, audio_cb, this);
        obs_source_release(to_release);
    }
    std::lock_guard<std::mutex> lock(m_mtx);
    shm_region_destroy(m_region);
    m_region = ShmRegion{};
}

bool TalkbackTap::is_open() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_open;
}

uint64_t TalkbackTap::last_audio_ms() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_last_audio_ms;
}

uint32_t TalkbackTap::sample_rate() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_sample_rate;
}

uint16_t TalkbackTap::channels() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_channels;
}

std::string TalkbackTap::region_name() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_region_name;
}

void TalkbackTap::audio_cb(void *param, obs_source_t *,
                           const struct audio_data *data, bool muted)
{
    static_cast<TalkbackTap *>(param)->on_audio(data, muted);
}

void TalkbackTap::on_audio(const struct audio_data *data, bool muted)
{
    if (!data || data->frames == 0) return;

    // A muted source still calls back, with real buffers. Publishing them
    // would put the director on air after they muted themselves -- exactly
    // the wrong direction for a fail-closed design. Publish silence instead
    // of nothing, so the dead-man switch does not read a mute as a dead path
    // and close the key.
    uint32_t rate, chans;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open || m_region.ptr == nullptr) return;
        rate  = m_sample_rate;
        chans = m_channels;
    }

    const std::size_t bytes = talkback_pcm_bytes(data->frames, chans);
    if (bytes == 0 || bytes > kTalkbackSlotBytes) return;

    std::vector<int16_t> pcm(data->frames * chans, 0);
    if (!muted) {
        const float *planes[2] = {
            reinterpret_cast<const float *>(data->data[0]),
            chans > 1 ? reinterpret_cast<const float *>(data->data[1]) : nullptr,
        };
        talkback_pcm_interleave(planes, data->frames, chans, pcm.data());
    }

    const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open || m_region.ptr == nullptr) return;
        notify = talkback_ring_publish(m_region.ptr, pcm.data(),
                                       static_cast<uint32_t>(bytes), now_ms);
        m_last_audio_ms = now_ms;
    }
    (void)rate;
    (void)notify;   // Task 6 sends the pipe event on this edge.
}
