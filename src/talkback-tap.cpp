#include "talkback-tap.h"
#include "talkback-pcm.h"
#include "talkback-ring.h"
#include "shm-generation.h"
#include "zoom-engine-client.h"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
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
    // F9 review-round fix: the spec's guarantee has two halves and only ONE
    // is structural (a capture callback observes and cannot route -- see
    // tests/talkback-isolation-test.cpp). The other half is the operator's:
    // OBS enables ALL SIX mixer tracks by default on every new audio source,
    // so an operator who points talkback at the mic already live on program
    // gets a working demo AND puts the aside on air, at full level, for the
    // audience. "Without the advisory half the guarantee is only half true.
    // Both ship together." This is that advisory floor -- a log warning, not
    // a refusal; the full dock UI is a later milestone.
    const uint32_t mixers = obs_source_get_audio_mixers(src);
    if (mixers != 0) {
        std::string tracks;
        for (int i = 0; i < 6; ++i) {
            if (mixers & (1u << i)) {
                if (!tracks.empty()) tracks += ", ";
                tracks += std::to_string(i + 1);
            }
        }
        blog(LOG_WARNING,
             "[obs-zoom-plugin] talkback: source \"%s\" has program track(s) "
             "%s enabled in Advanced Audio Properties. If any of those "
             "tracks are live on air, the audience will hear this talkback "
             "aside at full level. Uncheck its program tracks for a "
             "talkback-only source.",
             source_name.c_str(), tracks.c_str());
    }

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

    // MUST follow talkback_ring_init(), not precede it: the engine validates
    // slot_count/slot_bytes from the ring header when it maps this region,
    // and would reject a region it mapped before the header was laid out.
    ZoomEngineClient::instance().talkback_open(m_region_name, rate, chans);

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
    // Tell the engine before the region goes away, mirroring open()'s
    // ordering constraint in reverse: the engine must stop touching this
    // name before shm_region_destroy() below invalidates it.
    ZoomEngineClient::instance().talkback_close();
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
    if (bytes == 0) return;
    if (bytes > kTalkbackSlotBytes) {
        // F6 review-round fix: this used to return bare here -- no counter,
        // no log, no publish. Silence with no diagnostic means
        // last_audio_ms never advances, so the dead-man switch (see
        // src/talkback-key.h) closes the key ~250ms later with nothing in
        // the log to explain why: "talkback arms and instantly disarms,
        // every time, no error anywhere." The 8192-byte cap's rationale in
        // engine-ipc.h ("OBS delivers AUDIO_OUTPUT_FRAMES (1024) frames")
        // does not actually hold here -- a capture callback carries the
        // SOURCE's buffer, whatever the device period produced, not a fixed
        // OBS-internal frame count. Rate-limited: this can fire on every
        // callback while it's happening, and that is exactly the pipe/log
        // storm shape this codebase already has incidents about.
        static std::atomic<uint32_t> s_oversize_drops{0};
        const uint32_t n = ++s_oversize_drops;
        if (n == 1 || (n % 100) == 0) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] talkback: dropped an oversized audio "
                 "callback (%zu bytes > %u byte cap) -- %u drop(s) so far",
                 bytes, static_cast<unsigned>(kTalkbackSlotBytes), n);
        }
        return;
    }

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
    // F2 review-round fix: this memset MUST be unconditional, before muted
    // is even considered -- it used to live only in the `muted` branch (a
    // regression from an earlier fix round: the std::vector it replaced was
    // value-initialized, this stack array is not). talkback_pcm_interleave
    // has a documented refusal for a null plane ("a tap can fire with a null
    // plane during source teardown" -- see talkback-pcm.h), and that is
    // reachable well beyond teardown: an operator changing Settings > Audio
    // > Channels while a key is open rebuilds libobs's resampler, and
    // m_channels (cached at open(), never revalidated) can now disagree with
    // what data->data[] actually holds, handing back a null plane on a live
    // callback. Without an unconditional zero here, that refusal published
    // whatever 8KB of stack garbage happened to be sitting in `pcm` --
    // full-scale noise straight into a director's ear, the exact sound
    // talkback-pcm.h's clamp comment calls "the single worst sound to put in
    // a director's ear."
    std::memset(pcm, 0, bytes);
    if (!muted) {
        const float *planes[2] = {
            reinterpret_cast<const float *>(data->data[0]),
            chans > 1 ? reinterpret_cast<const float *>(data->data[1]) : nullptr,
        };
        if (!talkback_pcm_interleave(planes, data->frames, chans, pcm)) {
            // Refused -- pcm is already silence from the memset above, so
            // publishing it is safe, but a silent refusal is exactly what
            // F2 exists to stop being invisible. Count and log rather than
            // publish without a trace; rate-limited for the same reason as
            // the oversize-drop log above.
            static std::atomic<uint32_t> s_interleave_refusals{0};
            const uint32_t n = ++s_interleave_refusals;
            if (n == 1 || (n % 100) == 0) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] talkback: interleave refused (null "
                     "plane or degenerate input) -- publishing silence "
                     "instead, %u refusal(s) so far",
                     n);
            }
        }
    }

    // F10 review-round fix: capture_ns is documented in engine-ipc.h as
    // nanoseconds from os_gettime_ns() -- this used to pass now_ms
    // (milliseconds) into it. Harmless today because nothing reads the
    // field yet, but it is the one field whose comment explains its clock
    // domain, and a unit that silently changes meaning across the
    // plugin/engine boundary is exactly the kind of thing that stays
    // harmless right up until something DOES read it. m_last_audio_ms (the
    // dead-man switch's clock) still wants milliseconds, so both are kept.
    const uint64_t now_ns = os_gettime_ns();
    const uint64_t now_ms = now_ns / 1000000ULL;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open || m_region.ptr == nullptr) return;
        notify = talkback_ring_publish(m_region.ptr, pcm,
                                       static_cast<uint32_t>(bytes), now_ns);
        m_last_audio_ms = now_ms;
    }
    (void)rate;
    // THE EDGE, at last. talkback_ring_publish returns true exactly when this
    // publish crossed empty -> non-empty and one event must be sent. Sending
    // one per BUFFER instead would be ~100 pipe lines/sec -- the message-storm
    // shape this codebase has a live incident about, and the reason the ring
    // is edge-triggered at all.
    //
    // This runs on the OBS capture thread, so it must not block: write_json
    // is a non-blocking pipe write that drops on a broken link, and a dropped
    // edge is recovered by the dead-man switch closing the key rather than by
    // retrying here.
    if (notify) ZoomEngineClient::instance().talkback_audio();
}
