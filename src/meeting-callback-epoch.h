#pragma once
#include <atomic>
#include <cstdint>

// Captured on SDK callback arrival, checked when its main-queue work executes.
class MeetingCallbackEpoch {
    std::atomic<uint64_t> epoch{1};
    bool leaving = false; // begin/leave/deliver are SDK main-queue operations
    void advance() { epoch.fetch_add(1, std::memory_order_acq_rel); }
public:
    uint64_t capture() const { return epoch.load(std::memory_order_acquire); }
    void begin() { leaving = false; advance(); }
    void leave() { leaving = true; advance(); }
    template<class Callback> void deliver(uint64_t ticket, bool terminal, Callback callback) const {
        if (ticket == capture() && (!leaving || terminal)) callback();
    }
};
