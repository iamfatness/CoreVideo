#include "talkback-tap.h"
#include "talkback-pcm.h"
#include "talkback-ring.h"
#include "shm-generation.h"

#include <obs-module.h>
#include <util/platform.h>

#include <cstring>

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
        // Fold in the OS error code, not just "could not create": this is
        // exactly the class of failure the 2026-08-17 incident (ghost writer
        // sharing a ring, ~92% audio loss, no error anywhere) went unseen
        // for -- an operator staring at a generic message can't tell a
        // permissions problem from a name collision from disk pressure.
        error_out = "Could not create the talkback shared-memory region \"" +
                    m_region_name + "\" (error " +
                    std::to_string(m_region.last_error) + ")";
        obs_source_release(src);
        return false;
    }
    // shm_region_create() SUCCEEDS when it merely OPENED an existing section
    // instead of creating a fresh one -- see ShmRegion::already_existed's
    // comment in engine-ipc.h. That is precisely the shape of the
    // 2026-08-17 incident: a stale section from an orphaned process was
    // silently reopened and ghost-written, and nothing surfaced it until it
    // was root-caused live. talkback_ring_init() below resets the header
    // immediately, which limits the damage here, but "mitigated" is exactly
    // what was believed last time -- so this is surfaced loudly rather than
    // silently, even though open() still proceeds.
    if (m_region.already_existed) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] talkback: region \"%s\" already existed -- "
             "opened a section left by a previous process instead of "
             "creating a fresh one. Reinitializing its header now; if audio "
             "loss follows, an orphaned ZoomObsEngine or plugin instance may "
             "still be mapping this name.",
             m_region_name.c_str());
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

    // FIXED STACK BUFFER, not a per-callback heap allocation. This runs on
    // OBS's audio-mixer thread, shared with every other source's capture
    // callback, ~100 times/sec while a key is open -- exactly the class of
    // unbudgeted media-thread work this codebase has repeatedly root-caused
    // to live audio glitches (the ~1.06s FFmpeg preload taken under a lock
    // shared with the audio path; QProcess banned outright from media
    // threads -- see CLAUDE.md). The `bytes > kTalkbackSlotBytes` check
    // above already proves `bytes` fits before a single byte of this array
    // is touched, so the bound is established once, not re-derived here.
    int16_t pcm[kTalkbackSlotBytes / sizeof(int16_t)];
    if (muted) {
        // Silence must still be published at the correct length so the
        // dead-man switch doesn't read a mute as a dead path (see the
        // comment above). A stack array isn't value-initialized like the
        // vector this replaced, so the silence has to be zeroed explicitly.
        std::memset(pcm, 0, bytes);
    } else {
        const float *planes[2] = {
            reinterpret_cast<const float *>(data->data[0]),
            chans > 1 ? reinterpret_cast<const float *>(data->data[1]) : nullptr,
        };
        talkback_pcm_interleave(planes, data->frames, chans, pcm);
    }

    const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open || m_region.ptr == nullptr) return;
        notify = talkback_ring_publish(m_region.ptr, pcm,
                                       static_cast<uint32_t>(bytes), now_ms);
        m_last_audio_ms = now_ms;
    }
    (void)rate;
    (void)notify;   // Task 6 sends the pipe event on this edge.
}
