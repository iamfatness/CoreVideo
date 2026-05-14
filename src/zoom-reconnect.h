#pragma once

#include "zoom-types.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

struct ZoomReconnectPolicy {
    bool  enabled            = true;
    int   max_attempts       = 5;
    int   base_delay_ms      = 2000;
    int   max_delay_ms       = 30000;
    float backoff_multiplier = 2.0f;
    bool  on_engine_crash    = true;
    bool  on_disconnect      = true;
    bool  on_auth_fail       = false;
};

class ZoomReconnectManager {
public:
    static ZoomReconnectManager &instance();

    void set_policy(const ZoomReconnectPolicy &policy);
    ZoomReconnectPolicy policy() const;

    // Store the last session parameters used for recovery.
    void store_session(const std::string &jwt,
                       const std::string &meeting_id,
                       const std::string &passcode,
                       const std::string &display_name);

    // Called when an unexpected disconnect occurs.
    void trigger(RecoveryReason reason);
    // Called when the user deliberately cancels recovery.
    void cancel();
    // Called by ZoomEngineClient when a join succeeds.
    void on_join_success();
    // Called by ZoomEngineClient when a join attempt fails.
    // permanent == true means no further retries should be attempted.
    void on_join_failed(bool permanent);

    bool is_recovering() const { return m_recovering.load(std::memory_order_acquire); }
    int  attempt_count() const { return m_attempt.load(std::memory_order_acquire); }
    // Milliseconds until next retry, 0 when not waiting.
    int  next_retry_ms() const;

private:
    ZoomReconnectManager() = default;
    ~ZoomReconnectManager();

    void schedule_retry(int delay_ms);
    void execute_retry(); // always runs on OBS UI thread

    int compute_delay(int attempt) const;

    mutable std::mutex          m_mtx;
    ZoomReconnectPolicy         m_policy;
    std::string                 m_jwt;
    std::string                 m_meeting_id;
    std::string                 m_passcode;
    std::string                 m_display_name;

    std::atomic<bool>           m_recovering{false};
    std::atomic<int>            m_attempt{0};
    std::chrono::steady_clock::time_point m_retry_at;

    // Timer thread state (guarded by m_mtx)
    std::thread                 m_timer;
    std::condition_variable     m_cv;
    bool                        m_timer_cancel  = false;
    bool                        m_destroyed     = false;
};
