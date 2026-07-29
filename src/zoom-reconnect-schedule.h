#pragma once

#include <cstdint>

// Pure generation/pending bookkeeping for ZoomReconnectManager's retry
// scheduler, factored out for host-side unit testing without OBS/Qt/
// Zoom-SDK dependencies. NOT thread-safe by itself: ZoomReconnectManager
// only ever touches an instance of this class while holding its own m_mtx,
// exactly as it did the raw m_pending/m_generation fields before this
// refactor -- this class only centralizes the *rules*, not the locking.
//
// Contract (mirrors the original in-class fields byte-for-byte):
//  - schedule() marks a retry pending and bumps the generation, so any
//    earlier wake-up (or a previously queued execute_retry hand-off)
//    recognizes itself as stale via is_current().
//  - consume_pending() clears the pending flag WITHOUT bumping the
//    generation -- this is what the timer thread does right before handing
//    the fire-generation off to execute_retry() on the OBS UI thread, so
//    is_current() still recognizes that hand-off as valid.
//  - reset() clears pending and bumps the generation -- used on
//    cancel/give-up/success so any in-flight wake-up or already-queued
//    execute_retry() call becomes stale and no-ops.
class ZoomReconnectSchedule {
public:
    uint64_t schedule()
    {
        ++m_generation;
        m_pending = true;
        return m_generation;
    }

    void consume_pending() { m_pending = false; }

    uint64_t reset()
    {
        m_pending = false;
        ++m_generation;
        return m_generation;
    }

    bool pending() const { return m_pending; }
    uint64_t generation() const { return m_generation; }
    bool is_current(uint64_t generation) const { return m_generation == generation; }

private:
    bool     m_pending    = false;
    uint64_t m_generation = 0;
};
