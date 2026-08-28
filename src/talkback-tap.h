#pragma once
//
// talkback-tap.h — the plugin's talkback audio source and ring writer.
//
// Given an OBS source name, attaches obs_source_add_audio_capture_callback and
// publishes what it hears into the talkback ring for the engine to send.
//
// A CAPTURE CALLBACK IS A TAP, NOT A ROUTE. It observes a source's
// post-processing audio and cannot add that source to any mix. That is the
// structural half of the spec's guarantee that talkback never reaches program
// or ISO -- pinned by tests/talkback-isolation-test.cpp. The advisory half
// (warning when the chosen source is itself live on a program track) is the
// dock's job and is NOT implemented here.
//
// The tap is attached only while a key is open and detached the instant it
// closes, so an unkeyed talkback source costs nothing.
//
#include <cstdint>
#include <mutex>
#include <string>

#include "engine-ipc.h"   // ShmRegion, region helpers

struct obs_source;
typedef struct obs_source obs_source_t;
struct audio_data;

class TalkbackTap {
public:
    ~TalkbackTap();

    // Attach to `source_name` and create the ring. Returns false with a
    // human-readable reason in `error_out` -- an operator who picked a source
    // that cannot work needs to know WHICH reason, not that "talkback failed".
    bool open(const std::string &source_name, std::string &error_out);
    void close();
    bool is_open() const;

    // Monotonic ms of the last buffer published. The dead-man switch reads
    // this; see src/talkback-key.h.
    uint64_t last_audio_ms() const;

    uint32_t    sample_rate() const;
    uint16_t    channels() const;
    std::string region_name() const;

    // Set by open(); the engine is told this name so it can map the region.
    static const char *base_region_name() { return "ZoomObsPlugin_talkback"; }

private:
    static void audio_cb(void *param, obs_source_t *source,
                         const struct audio_data *data, bool muted);
    void on_audio(const struct audio_data *data, bool muted);

    mutable std::mutex m_mtx;
    obs_source_t *m_source      = nullptr;   // strong ref while open
    ShmRegion     m_region{};
    std::string   m_region_name;
    uint32_t      m_sample_rate = 0;
    uint16_t      m_channels    = 0;
    uint64_t      m_last_audio_ms = 0;
    bool          m_open        = false;
};
