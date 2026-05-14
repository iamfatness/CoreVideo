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

ZoomReconnectManager::~ZoomReconnectManager()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_timer_cancel = true;
        m_destroyed    = true;
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

int ZoomReconnectManager::compute_delay(int attempt) const
{
    // Already holds m_mtx.
    double delay = m_policy.base_delay_ms *
                   std::pow(static_cast<double>(m_policy.backoff_multiplier), attempt);
    return static_cast<int>(std::min(delay, static_cast<double>(m_policy.max_delay_ms)));
}

int ZoomReconnectManager::next_retry_ms() const
{
    if (!m_recovering.load(std::memory_order_acquire)) return 0;
    using namespace std::chrono;
    std::lock_guard<std::mutex> lk(m_mtx);
    auto remaining = duration_cast<milliseconds>(m_retry_at - steady_clock::now());
    return std::max(0, static_cast<int>(remaining.count()));
}

void ZoomReconnectManager::trigger(RecoveryReason reason)
{
    std::unique_lock<std::mutex> lk(m_mtx);

    if (!m_policy.enabled) {
        blog(LOG_WARNING, "[obs-zoom-plugin] Reconnect disabled; not recovering");
        return;
    }

    // Check per-reason policy.
    switch (reason) {
    case RecoveryReason::LicenseError:
        blog(LOG_ERROR, "[obs-zoom-plugin] License error — reconnect not attempted");
        return;
    case RecoveryReason::AuthFailure:
        if (!m_policy.on_auth_fail) {
            blog(LOG_ERROR, "[obs-zoom-plugin] Auth failure — reconnect suppressed by policy");
            return;
        }
        break;
    case RecoveryReason::HostEndedMeeting:
        blog(LOG_INFO, "[obs-zoom-plugin] Host ended meeting — not reconnecting");
        // Set Idle, not Failed.
        ZoomEngineClient::instance().set_state(MeetingState::Idle);
        return;
    case RecoveryReason::EngineCrash:
        if (!m_policy.on_engine_crash) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Engine crash — reconnect suppressed by policy");
            return;
        }
        break;
    case RecoveryReason::MeetingDisconnect:
    case RecoveryReason::NetworkDrop:
    case RecoveryReason::SdkError:
        if (!m_policy.on_disconnect) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Disconnect — reconnect suppressed by policy");
            return;
        }
        break;
    }

    if (m_meeting_id.empty()) {
        blog(LOG_WARNING, "[obs-zoom-plugin] No stored session — cannot reconnect");
        return;
    }

    const int attempt = m_attempt.load(std::memory_order_relaxed);
    if (attempt >= m_policy.max_attempts) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Max reconnect attempts (%d) reached — giving up",
             m_policy.max_attempts);
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        m_recovering.store(false, std::memory_order_release);
        return;
    }

    m_recovering.store(true, std::memory_order_release);
    ZoomEngineClient::instance().set_state(MeetingState::Recovering);

    const int delay = compute_delay(attempt);
    blog(LOG_INFO, "[obs-zoom-plugin] Scheduling reconnect attempt %d/%d in %d ms",
         attempt + 1, m_policy.max_attempts, delay);

    schedule_retry(delay);
}

void ZoomReconnectManager::cancel()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_timer_cancel = true;
        m_recovering.store(false, std::memory_order_release);
        m_attempt.store(0, std::memory_order_release);
    }
    m_cv.notify_all();
    blog(LOG_INFO, "[obs-zoom-plugin] Recovery cancelled by user");
    ZoomEngineClient::instance().set_state(MeetingState::Idle);
}

void ZoomReconnectManager::on_join_success()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_timer_cancel = true;
        m_recovering.store(false, std::memory_order_release);
        m_attempt.store(0, std::memory_order_release);
    }
    m_cv.notify_all();

    blog(LOG_INFO, "[obs-zoom-plugin] Reconnect succeeded — resubscribing all outputs");
    // Re-subscribe all sources so they receive video/audio again.
    ZoomOutputManager::instance().resubscribe_all();
}

void ZoomReconnectManager::on_join_failed(bool permanent)
{
    if (permanent) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Permanent join failure — not retrying");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_recovering.store(false, std::memory_order_release);
            m_attempt.store(0, std::memory_order_release);
        }
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        return;
    }

    // Increment attempt counter and schedule next retry.
    const int attempt = m_attempt.fetch_add(1, std::memory_order_acq_rel) + 1;

    std::unique_lock<std::mutex> lk(m_mtx);
    if (attempt >= m_policy.max_attempts) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Reconnect attempt %d/%d failed — giving up",
             attempt, m_policy.max_attempts);
        m_recovering.store(false, std::memory_order_release);
        m_attempt.store(0, std::memory_order_release);
        lk.unlock();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        return;
    }

    const int delay = compute_delay(attempt);
    blog(LOG_WARNING, "[obs-zoom-plugin] Reconnect attempt %d/%d failed — retrying in %d ms",
         attempt, m_policy.max_attempts, delay);

    ZoomEngineClient::instance().set_state(MeetingState::Recovering);
    schedule_retry(delay);
}

void ZoomReconnectManager::schedule_retry(int delay_ms)
{
    // Must be called with m_mtx held.
    m_timer_cancel = true;
    m_cv.notify_all();
    if (m_timer.joinable()) m_timer.detach(); // detach; cancellation via m_timer_cancel

    m_timer_cancel = false;
    m_retry_at = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(delay_ms);

    m_timer = std::thread([this, delay_ms]() {
        std::unique_lock<std::mutex> lk(m_mtx);
        const bool cancelled = m_cv.wait_for(
            lk, std::chrono::milliseconds(delay_ms),
            [this]() { return m_timer_cancel || m_destroyed; });
        const bool destroyed = m_destroyed;
        lk.unlock();

        if (!cancelled && !destroyed) {
            obs_queue_task(OBS_TASK_UI, [](void *param) {
                static_cast<ZoomReconnectManager *>(param)->execute_retry();
            }, this, false);
        }
    });
}

void ZoomReconnectManager::execute_retry()
{
    // Runs on the OBS UI thread.
    if (!m_recovering.load(std::memory_order_acquire)) return;

    std::string jwt, meeting_id, passcode, display_name;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_timer_cancel) return; // was cancelled between scheduling and execution
        jwt          = m_jwt;
        meeting_id   = m_meeting_id;
        passcode     = m_passcode;
        display_name = m_display_name;
    }

    const int attempt = m_attempt.fetch_add(1, std::memory_order_acq_rel) + 1;
    blog(LOG_INFO, "[obs-zoom-plugin] Executing reconnect attempt %d", attempt);

    // Bring down whatever state remains, then bring it back up.
    ZoomEngineClient::instance().stop();

    if (!ZoomEngineClient::instance().start(jwt)) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Failed to start engine on reconnect attempt %d",
             attempt);
        on_join_failed(false);
        return;
    }

    ZoomEngineClient::instance().join(meeting_id, passcode, display_name);
    // Success/failure reported via on_join_success() / on_join_failed() from handle_event().
}
