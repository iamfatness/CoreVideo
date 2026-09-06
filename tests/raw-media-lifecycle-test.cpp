#include "raw-media-lifecycle.h"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

static int failures = 0;
static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
// Count external effects, including the CURRENT desired output set at restore.
// Removing a binding while suspended must not be undone by a captured retry set.
struct Harness {
    RawMediaLifecycle lifecycle;
    int checks = 0, requests = 0, starts = 0, restores = 0, suspends = 0, errors = 0;
    bool permission = false, start_ok = true;
    std::set<std::string> desired{"A", "B"}, restored;
    void send(RawMediaEvent e) {
        switch (lifecycle.on(e)) {
        case RawMediaAction::Check: ++checks; send(permission ? RawMediaEvent::CheckReady : RawMediaEvent::NoPermission); break;
        case RawMediaAction::Request: ++requests; break;
        case RawMediaAction::Start: ++starts; send(start_ok ? RawMediaEvent::StartSucceeded : RawMediaEvent::StartFailed); break;
        case RawMediaAction::Restore: ++restores; restored = desired; break;
        case RawMediaAction::Suspend: ++suspends; restored.clear(); break;
        case RawMediaAction::Fail: ++errors; break;
        default: break;
        }
    }
    void join() { send(RawMediaEvent::InMeeting); }
    void grant() { permission = true; send(RawMediaEvent::Grant); }
};
int main() {
    {
        Harness h; h.join(); h.send(RawMediaEvent::Start); h.send(RawMediaEvent::Start);
        check(h.requests == 1 && h.checks == 1 && h.starts == 0, "duplicate pending Start has one host request and one check");
        check(std::string(h.lifecycle.state()) == "waiting_permission", "pending remains visible");
        h.send(RawMediaEvent::Denied); h.grant(); h.grant();
        check(h.starts == 1 && h.restores == 1, "denial then both grant callbacks start and restore once");
    }
    {
        Harness h; h.join(); h.send(RawMediaEvent::Start); h.send(RawMediaEvent::Stop); h.grant();
        check(h.starts == 0 && h.restores == 0, "late grant cannot resurrect explicit Stop");
    }
    {
        Harness h; h.join(); h.permission = true; h.send(RawMediaEvent::Start);
        h.send(RawMediaEvent::Transition); h.send(RawMediaEvent::Transition);
        h.desired.erase("B"); h.grant();
        check(h.starts == 1 && h.suspends == 1, "transition suspends once and grant cannot start inside transfer");
        h.join(); h.join();
        check(h.checks == 2 && h.starts == 2 && h.restores == 2, "room readiness rechecks and restores exactly once");
        check(h.restored == std::set<std::string>{"A"}, "restore uses current assignments after source removal");
    }
    {
        Harness h; h.join(); h.permission = true; h.send(RawMediaEvent::Start);
        h.send(RawMediaEvent::Denied);
        check(h.suspends == 1 && std::string(h.lifecycle.state()) == "denied", "revocation invalidates active readiness");
        h.grant(); h.grant();
        check(h.starts == 2 && h.restores == 2, "revocation then grant recovers once");
    }
    {
        Harness h; h.join(); h.permission = true; h.start_ok = false; h.send(RawMediaEvent::Start);
        for (int i = 0; i < 20; ++i) { h.grant(); h.join(); }
        check(h.starts == 1 && h.errors == 1 && h.restores == 0, "permanent start failure has finite attempts and terminal error");
        check(std::string(h.lifecycle.state()) == "failed", "terminal failure remains visible");
    }
    {
        Harness h; h.join(); h.send(RawMediaEvent::Start); h.send(RawMediaEvent::Reset); h.grant(); h.join();
        check(h.starts == 0, "replacement meeting cancels old intent");
    }
    {
        Harness h; h.join(); h.send(RawMediaEvent::Start); h.send(RawMediaEvent::Timeout);
        check(std::string(h.lifecycle.state()) == "waiting_permission", "timeout is still awaiting host permission, not denial");
        h.grant(); h.send(RawMediaEvent::Timeout);
        check(h.starts == 1 && h.suspends == 0 && std::string(h.lifecycle.state()) == "active", "stale request timeout cannot revoke a grant");
    }
    {
        Harness h; h.send(RawMediaEvent::Start); h.grant();
        check(h.starts == 0 && h.checks == 0, "Start and grant wait for actual meeting readiness");
        h.join();
        check(h.starts == 1, "queued Start runs once meeting becomes ready");
    }
    {
        Harness h; h.join(); h.send(RawMediaEvent::Start); h.send(RawMediaEvent::Stop);
        h.send(RawMediaEvent::Start);
        check(h.requests == 1, "manual Stop/Start cannot repeatedly prompt the host");
        h.send(RawMediaEvent::Reset); h.join(); h.send(RawMediaEvent::Start);
        check(h.requests == 2, "a replacement meeting has its own request budget");
    }
    {
        RawMediaLifecycle lifecycle;
        lifecycle.on(RawMediaEvent::InMeeting);
        lifecycle.on(RawMediaEvent::Start);
        check(lifecycle.on(RawMediaEvent::CheckFailed) == RawMediaAction::Fail,
              "non-permission check failure reports a terminal error without asking host");
        check(lifecycle.on(RawMediaEvent::Grant) == RawMediaAction::None,
              "unsolicited grant cannot retry a permanent check failure");
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
