#pragma once

// Decisions only; the macOS SDK main queue owns this state and every effect.
// Grants authorize an existing Start intent, never create one. Room readiness
// is independent of permission and starts a fresh check after a transfer.
enum class RawMediaEvent { Start, Stop, Reset, InMeeting, Transition, Grant,
    Denied, Timeout, NoPermission, CheckReady, CheckFailed, StartSucceeded, StartFailed,
    RequestFailed };
enum class RawMediaAction { None, Check, Start, Request, Suspend, Restore, Fail };
class RawMediaLifecycle {
    enum class Phase { Stopped, Waiting, Denied, Recovering, Checking, Starting, Active, Failed };
    Phase phase = Phase::Stopped;
    bool wanted = false, in_meeting = false, requested_permission = false;
    bool granted = false;
    RawMediaAction check() {
        phase = Phase::Checking;
        return RawMediaAction::Check;
    }
public:
    const char *state() const {
        switch (phase) {
        case Phase::Stopped: return "stopped";
        case Phase::Waiting: return "waiting_permission";
        case Phase::Denied: return "denied";
        case Phase::Recovering: return "recovering";
        case Phase::Checking: case Phase::Starting: return "starting";
        case Phase::Active: return "active";
        case Phase::Failed: return "failed";
        }
        return "failed";
    }
    RawMediaAction on(RawMediaEvent e) {
        switch (e) {
        case RawMediaEvent::Reset:
            *this = RawMediaLifecycle{};
            return RawMediaAction::None;
        case RawMediaEvent::Stop:
            wanted = false; phase = Phase::Stopped;
            // One request per meeting, including manual Stop/Start cycles.
            return RawMediaAction::None;
        case RawMediaEvent::Start:
            if (wanted && phase != Phase::Failed) return RawMediaAction::None;
            wanted = true;
            if (in_meeting) return check();
            phase = Phase::Recovering;
            return RawMediaAction::None;
        case RawMediaEvent::InMeeting:
            if (in_meeting) return RawMediaAction::None;
            in_meeting = true;
            if (wanted) return check();
            return RawMediaAction::None;
        case RawMediaEvent::Transition: {
            in_meeting = false;
            const bool was_active = phase == Phase::Active;
            if (wanted) phase = Phase::Recovering;
            return was_active ? RawMediaAction::Suspend : RawMediaAction::None;
        }
        case RawMediaEvent::Denied: {
            granted = false;
            const bool was_active = phase == Phase::Active;
            if (wanted) phase = Phase::Denied;
            return was_active ? RawMediaAction::Suspend : RawMediaAction::None;
        }
        case RawMediaEvent::Timeout:
            // A timed-out request is not a denial, and cannot revoke an
            // already-established grant delivered through the other callback.
            return RawMediaAction::None;
        case RawMediaEvent::Grant:
            if (granted) return RawMediaAction::None;
            granted = true;
            if (wanted && in_meeting && (phase == Phase::Waiting || phase == Phase::Denied || phase == Phase::Recovering)) return check();
            return RawMediaAction::None;
        case RawMediaEvent::NoPermission:
            if (phase != Phase::Checking) return RawMediaAction::None;
            granted = false;
            phase = Phase::Waiting;
            if (requested_permission) return RawMediaAction::None;
            requested_permission = true;
            return RawMediaAction::Request;
        case RawMediaEvent::CheckReady:
            if (phase != Phase::Checking) return RawMediaAction::None;
            granted = true; phase = Phase::Starting;
            return RawMediaAction::Start;
        case RawMediaEvent::StartSucceeded:
            if (phase != Phase::Starting) return RawMediaAction::None;
            phase = Phase::Active;
            return RawMediaAction::Restore;
        case RawMediaEvent::CheckFailed: case RawMediaEvent::StartFailed: case RawMediaEvent::RequestFailed:
            if (!wanted) return RawMediaAction::None;
            phase = Phase::Failed;
            return RawMediaAction::Fail;
        }
        return RawMediaAction::None;
    }
};
