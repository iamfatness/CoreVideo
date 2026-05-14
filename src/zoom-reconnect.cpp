#include "zoom-reconnect.h"
#include "zoom-engine-client.h"
#include "zoom-output-manager.h"
#include <obs-module.h>
#include <algorithm>
#include <cmath>

ZoomReconnectManager &ZoomReconnectManager::instance()
{
    static ZoomReconnectManager inst;
    return inst;
}

ZoomReconnectManager::ZoomReconnectManager()
{
    m_timer = std::thread([this]() { timer_loop(); });
}

ZoomReconnectManager::~ZoomReconnectManager()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop_thread = true;
        m_pending     = false;
        ++m_generation;
    }
    m_cv.notify_all();
    if (m_timer.joinable()) m_timer.join();
}

void ZoomReconnectManager::set_policy(const ZoomReconnectPolicy &policy)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_policy = policy;
}

ZoomReconnectPolicy ZoomReconnectManager::policy() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_policy;
}

void ZoomReconnectManager::store_session(const std::string &jwt,
                                         const std::string &meeting_id,
                                         const std::string &passcode,
                                         const std::string &display_name)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_jwt          = jwt;
    m_meeting_id   = meeting_id;
    m_passcode     = passcode;
    m_display_name = display_name;
}

void ZoomReconnectManager::clear_session()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    // Wipe credentials we no longer need (defensive hygiene).
    m_jwt.assign(m_jwt.size(), '\0');
    m_jwt.clear();
    m_meeting_id.clear();
    m_passcode.assign(m_passcode.size(), '\0');
    m_passcode.clear();
    m_display_name.clear();
}

int ZoomReconnectManager::compute_delay_locked(int attempt) const
{
    double delay = m_policy.base_delay_ms *
                   std::pow(static_cast<double>(m_policy.backoff_multiplier), attempt);
    return static_cast<int>(std::min(delay, static_cast<double>(m_policy.max_delay_ms)));
}

int ZoomReconnectManager::next_retry_ms() const
{
    if (!m_recovering.load(std::memory_order_acquire)) return 0;
    using namespace std::chrono;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pending) return 0;
    auto remaining = duration_cast<milliseconds>(m_retry_at - steady_clock::now());
    return std::max(0, static_cast<int>(remaining.count()));
}

void ZoomReconnectManager::reset_state_locked()
{
    m_pending = false;
    ++m_generation;
    m_recovering.store(false, std::memory_order_release);
    m_attempt.store(0, std::memory_order_release);
}

void ZoomReconnectManager::trigger(RecoveryReason reason)
{
    std::unique_lock<std::mutex> lk(m_mtx);

    if (!m_policy.enabled) {
        blog(LOG_WARNING, "[obs-zoom-plugin] Reconnect disabled; not recovering");
        reset_state_locked();
        lk.unlock();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        return;
    }

    auto give_up = [&](const char *msg, MeetingState final_state) {
        blog(LOG_INFO, "[obs-zoom-plugin] %s", msg);
        reset_state_locked();
        lk.unlock();
        ZoomEngineClient::instance().set_state(final_state);
    };

    switch (reason) {
    case RecoveryReason::LicenseError:
        give_up("License error - reconnect not attempted", MeetingState::Failed);
        return;
    case RecoveryReason::AuthFailure:
        if (!m_policy.on_auth_fail) {
            give_up("Auth failure - reconnect suppressed by policy", MeetingState::Failed);
            return;
        }
        break;
    case RecoveryReason::HostEndedMeeting:
        give_up("Host ended meeting - not reconnecting", MeetingState::Idle);
        return;
    case RecoveryReason::EngineCrash:
        if (!m_policy.on_engine_crash) {
            give_up("Engine crash - reconnect suppressed by policy",
                    MeetingState::Failed);
            return;
        }
        break;
    case RecoveryReason::MeetingDisconnect:
    case RecoveryReason::NetworkDrop:
    case RecoveryReason::SdkError:
        if (!m_policy.on_disconnect) {
            give_up("Disconnect - reconnect suppressed by policy",
                    MeetingState::Failed);
            return;
        }
        break;
    }

    if (m_meeting_id.empty()) {
        give_up("No stored session - cannot reconnect", MeetingState::Failed);
        return;
    }

    const int attempt = m_attempt.load(std::memory_order_relaxed);
    if (attempt >= m_policy.max_attempts) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Max reconnect attempts (%d) reached",
             m_policy.max_attempts);
        reset_state_locked();
        lk.unlock();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        return;
    }

    m_recovering.store(true, std::memory_order_release);
    const int delay = compute_delay_locked(attempt);
    blog(LOG_INFO, "[obs-zoom-plugin] Scheduling reconnect attempt %d/%d in %d ms",
         attempt + 1, m_policy.max_attempts, delay);

    schedule_retry_locked(delay);
    lk.unlock();
    ZoomEngineClient::instance().set_state(MeetingState::Recovering);
}

void ZoomReconnectManager::cancel()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_recovering.load(std::memory_order_acquire) && !m_pending) return;
        reset_state_locked();
    }
    m_cv.notify_all();
    blog(LOG_INFO, "[obs-zoom-plugin] Recovery cancelled");
    ZoomEngineClient::instance().set_state(MeetingState::Idle);
}

void ZoomReconnectManager::on_join_success()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        reset_state_locked();
    }
    m_cv.notify_all();

    blog(LOG_INFO, "[obs-zoom-plugin] Reconnect succeeded - resubscribing outputs");
    ZoomOutputManager::instance().resubscribe_all();
}

void ZoomReconnectManager::on_join_failed(bool permanent)
{
    if (permanent) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Permanent join failure - not retrying");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            reset_state_locked();
        }
        m_cv.notify_all();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        return;
    }

    std::unique_lock<std::mutex> lk(m_mtx);

    // Count this attempt as consumed.
    const int attempt = m_attempt.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (attempt >= m_policy.max_attempts) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Reconnect attempt %d/%d failed - giving up",
             attempt, m_policy.max_attempts);
        reset_state_locked();
        lk.unlock();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        return;
    }

    const int delay = compute_delay_locked(attempt);
    blog(LOG_WARNING, "[obs-zoom-plugin] Reconnect attempt %d/%d failed - retrying in %d ms",
         attempt, m_policy.max_attempts, delay);

    m_recovering.store(true, std::memory_order_release);
    schedule_retry_locked(delay);
    lk.unlock();
    ZoomEngineClient::instance().set_state(MeetingState::Recovering);
}

void ZoomReconnectManager::schedule_retry_locked(int delay_ms)
{
    // Must be called with m_mtx held. Bumps generation so any previously
    // scheduled wake-up that races to fire will see the new generation and
    // ignore itself.
    ++m_generation;
    m_pending  = true;
    m_retry_at = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(delay_ms);
    m_cv.notify_all();
}

void ZoomReconnectManager::timer_loop()
{
    std::unique_lock<std::mutex> lk(m_mtx);
    while (!m_stop_thread) {
        if (!m_pending) {
            m_cv.wait(lk, [this]() { return m_stop_thread || m_pending; });
            continue;
        }

        const uint64_t gen      = m_generation;
        const auto      deadline = m_retry_at;
        // Sleep until deadline, but wake early if state changes.
        m_cv.wait_until(lk, deadline, [this, gen]() {
            return m_stop_thread || !m_pending || m_generation != gen;
        });

        if (m_stop_thread) break;
        if (!m_pending || m_generation != gen) continue; // cancelled or rescheduled
        if (std::chrono::steady_clock::now() < m_retry_at) continue; // spurious wake

        // Consume the pending request before scheduling on UI thread.
        m_pending = false;
        const uint64_t fire_gen = m_generation;
        lk.unlock();

        // We pass the generation through to execute_retry so it can verify
        // that a cancel() did not race between this point and OBS dispatching
        // the task on the UI thread.
        struct Payload { ZoomReconnectManager *mgr; uint64_t gen; };
        auto *payload = new Payload{this, fire_gen};
        obs_queue_task(OBS_TASK_UI, [](void *p) {
            auto *pl = static_cast<Payload *>(p);
            pl->mgr->execute_retry(pl->gen);
            delete pl;
        }, payload, false);

        lk.lock();
    }
}

void ZoomReconnectManager::execute_retry(uint64_t generation)
{
    // Runs on the OBS UI thread.
    std::string jwt, meeting_id, passcode, display_name;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_generation != generation) return;     // cancelled or superseded
        if (!m_recovering.load(std::memory_order_acquire)) return;
        jwt          = m_jwt;
        meeting_id   = m_meeting_id;
        passcode     = m_passcode;
        display_name = m_display_name;
    }

    // Note: the actual "attempt N" log message has already been emitted by
    // trigger() / on_join_failed(). Do NOT increment m_attempt here -- the
    // counter is advanced exactly once per attempt, when the attempt is
    // scheduled. Incrementing here would double-count.
    blog(LOG_INFO, "[obs-zoom-plugin] Executing scheduled reconnect");

    ZoomEngineClient::instance().stop_for_reconnect();
    if (!ZoomEngineClient::instance().start(jwt)) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Failed to start engine on reconnect");
        on_join_failed(false);
        return;
    }
    ZoomEngineClient::instance().join(meeting_id, passcode, display_name);
    // Result reported via on_join_success() / on_join_failed() from handle_event().
}
