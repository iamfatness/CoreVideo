// Exercises the real JSON event boundary: a room joined report must not
// retire assignments that the engine restores without another subscribe.
#include "engine-ipc.h"
#include "media-event-queue.h"
#include "media-failure-state.h"
#include "talkback-nomination.h"
#include "zoom-types.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
// Test-only access to the reader boundary and offline running flag.
#define private public
#include "zoom-engine-client.h"
#undef private
#include "zoom-reconnect.h"
#include <obs-module.h>
#include <util/platform.h>
#include <cstdio>
#include <cstdlib>

// External host boundaries only. No engine is launched, OBS initialized, or
// reconnect timer scheduled. Client parsing/state/callbacks remain production.
extern "C" void blog(int, const char *, ...) {}
extern "C" uint64_t os_gettime_ns() { return 1000000000ULL; }
extern "C" void bfree(void *p) { free(p); }
obs_module_t *obs_current_module() { return nullptr; }
extern "C" char *obs_find_module_file(obs_module_t *, const char *) { return nullptr; }
ZoomReconnectManager::ZoomReconnectManager() = default;
ZoomReconnectManager::~ZoomReconnectManager() = default;
ZoomReconnectManager &ZoomReconnectManager::instance() { static ZoomReconnectManager r; return r; }
void ZoomReconnectManager::cancel() {}
void ZoomReconnectManager::clear_session() {}
void ZoomReconnectManager::on_join_success() {}
void ZoomReconnectManager::on_join_failed(bool) {}
void ZoomReconnectManager::trigger(RecoveryReason) {}
void ZoomReconnectManager::store_session(const std::string &, const std::string &,
    const std::string &, const std::string &, MeetingKind, const ZoomJoinAuthTokens &) {}

static int failures;
static void check(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
int main() {
    auto &client = ZoomEngineClient::instance();
    client.m_running.store(true); // Invalid pipe drops writes; no launch needed.
    auto event = [&](const char *json) { client.handle_event(json); };
    auto fail = [&] { event(R"({"cmd":"error","msg":"video_subscribe_failed","source_uuid":"fixed","participant_id":42,"code":1})"); };
    event(R"({"cmd":"participants","participants":[{"id":42,"name":"Panelist"}]})");
    client.register_source("fixed", {});
    client.subscribe("fixed", 42, false);
    const auto ticket = client.media_delivery_ticket("fixed", 42);
    check(ticket != 0, "subscription registers a delivery ticket");
    fail();
    check(client.source_media_failed("fixed"), "initial source failure recognized");
    int fatal_notices = 0;
    int cleared_notices = 0;
    client.add_error_callback(&failures, [&](const std::string &message) {
        if (message.empty()) ++cleared_notices; else ++fatal_notices;
    });
    event(R"({"cmd":"error","msg":"meeting_failed","code":4})");
    check(fatal_notices == 1 && !client.last_error().empty(), "fatal meeting error retains callback route");
    event(R"({"cmd":"joined"})");
    check(cleared_notices == 1 && client.last_error().empty(), "joined clears independent connection error");
    event(R"({"cmd":"joined"})");
    event(R"({"cmd":"debug","stage":"raw_media_ready"})");
    check(client.is_media_active(), "raw media readiness traverses production debug dispatch");
    check(client.source_media_failed("fixed"), "duplicate joined and room readiness retain unresolved failure");
    check(client.media_delivery_ticket("fixed", 42) == ticket, "engine restore retains assignment without client subscribe");
    client.acknowledge_media_delivery("fixed", 42, ticket);
    check(!client.source_media_failed("fixed"), "acknowledged delivery clears recovered video");
    fail();
    check(client.source_media_failed("fixed"), "later engine-side restore failure still recognized");
    client.acknowledge_media_delivery("fixed", 42, ticket);
    event(R"({"cmd":"error","msg":"shm_create_failed","source_uuid":"fixed"})");
    event(R"({"cmd":"joined"})");
    check(client.source_media_failed("fixed"), "joined retains persistent source error");
    client.unregister_source("fixed");
    check(!client.source_media_failed("fixed"), "source removal retires persistent failure");
    client.subscribe("fixed", 42, false);
    fail();
    event(R"({"cmd":"left"})");
    check(!client.source_media_failed("fixed") && !client.media_delivery_ticket("fixed", 42), "true leave resets failure and assignment");
    event(R"({"cmd":"joined"})");
    check(!client.media_delivery_ticket("fixed", 42), "new joined does not resurrect old session assignment");
    event(R"({"cmd":"participants","participants":[{"id":42,"name":"Panelist"}]})");
    client.subscribe("fixed", 42, false);
    fail();
    check(client.source_media_failed("fixed"), "new session subscription recognizes failures");
    client.join("123", "", "test", MeetingKind::Meeting, {});
    check(!client.source_media_failed("fixed") && !client.media_delivery_ticket("fixed", 42), "explicit new join resets old media session");
    client.subscribe("fixed", 42, false);
    fail();
    client.remove_error_callback(&failures);
    client.m_running.store(false);
    client.stop();
    check(!client.source_media_failed("fixed") && !client.media_delivery_ticket("fixed", 42), "engine stop resets media session");
    return failures ? 1 : 0;
}
