#pragma once

// The coalescing queue between the engine-event reader thread and the media
// dispatch lanes. Pure std C++ so it can be tested without Qt, libobs or a
// live engine -- the same treatment audio-timeline.h gets, for the same
// reason: its only failure symptom is broken audio on air.
//
// THE DEFECT THIS EXISTS FOR (2026-08-17, full-1080p pressure test). The pipe
// reader thread used to run every media callback INLINE: each video "frame"
// event triggered the whole SHM->OBS copy (~3MB at 1080p) before the next
// pipe line was read. With Zoom granting 4x1920x1080 + 3x1600x900 feeds the
// thread fell behind its own pipe; audio events queued BEHIND video events
// and arrived measured 0.5-0.94s late, which starved the audio ring's
// edge-triggered wakeups into the 2.5s writer-keepalive pattern: ~92% audio
// loss on every source, indistinguishable on air from the ghost-writer wedge.
//
// The queue exploits a property both media handlers already have: events are
// PROMPTS, not payloads. A video handler reads the NEWEST frame in the shared
// region; an audio handler drains EVERYTHING pending in the ring. So N queued
// events for one source collapse into one dispatch with the latest event's
// parameters, and a backlog physically cannot form -- the queue's size is
// bounded by the number of live sources, never by the event rate.
//
// Single producer (the pipe reader), single consumer (a dispatch lane), but
// locked anyway: the cost is nanoseconds against a 10ms media cadence, and it
// keeps the class safe if a second producer ever appears.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The callback parameters of one media event, meaning-free at this layer.
// frame: p1=w, p2=h, p3=participant_id, p4=shm_gen
// audio: p1=byte_len, p2=participant_id, p3=shm_gen, p4 unused
struct MediaEvent {
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t p3 = 0;
    uint32_t p4 = 0;
};

class MediaEventQueue {
public:
    // Queues an event for `uuid`, replacing any event already pending for it.
    // Returns true when the uuid was NOT already pending -- i.e. the caller
    // must wake the consumer. Returns false when the event was coalesced onto
    // an existing entry: the consumer already has a wakeup coming, and waking
    // it again would only burn a syscall per 10ms buffer, which is the exact
    // load profile this queue exists to shed.
    bool push(const std::string &uuid, const MediaEvent &event)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_index.find(uuid);
        if (it != m_index.end()) {
            m_pending[it->second].second = event;
            ++m_coalesced;
            return false;
        }
        m_index.emplace(uuid, m_pending.size());
        m_pending.emplace_back(uuid, event);
        return true;
    }

    // Moves out everything pending, in the order each uuid FIRST became
    // pending. First-pending order is the fairness property: a source whose
    // events keep coalescing cannot push itself ahead of one that has been
    // waiting longer.
    std::vector<std::pair<std::string, MediaEvent>> drain()
    {
        std::vector<std::pair<std::string, MediaEvent>> out;
        std::lock_guard<std::mutex> lk(m_mtx);
        out.swap(m_pending);
        m_index.clear();
        return out;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_pending.empty();
    }

    // Events absorbed into an already-pending entry since construction. This
    // is the queue's effectiveness meter: a non-zero rate under load is the
    // system shedding exactly the work that used to drown the reader thread.
    uint64_t coalesced() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_coalesced;
    }

private:
    mutable std::mutex m_mtx;
    std::vector<std::pair<std::string, MediaEvent>> m_pending;
    std::unordered_map<std::string, size_t> m_index;
    uint64_t m_coalesced = 0;
};
