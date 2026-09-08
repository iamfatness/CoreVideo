#include "media-failure-state.h"
#include "zoom-engine-error-dispatch.h"
#include <iostream>
#include <cstdlib>
static void check(bool value, const char *name) { if (!value) { std::cerr << name << '\n'; std::exit(1); } }
int main() {
    check(zoom_source_video_failure("raw_data_controller_unavailable"), "raw controller unavailable belongs to source media recovery");
    check(!zoom_source_video_failure("meeting_failed") && !zoom_source_video_failure("auth_fail"), "meeting/auth errors retain fatal classification");
    MediaFailureState state;
    int diagnostics = 0, notices = 0, fatal = 0;
    for (int i=1; i<=8; ++i) state.assign(std::to_string(i), i);
    for (int batch=0; batch<3; ++batch) for (int i=1; i<=8; ++i) {
        ++diagnostics;
        if (state.fail(std::to_string(i), i, "SDK code 5", 100 + batch, true)) ++notices;
    }
    check(diagnostics == 24 && notices == 1, "24 diagnostics produce one episode notice");
    check(state.size() == 8 && state.terminal(103), "three failed attempts are actionable");
    const auto old = state.ticket("1", 1);
    state.assign("1", 9);
    state.fail("1", 9, "SDK code 5", 104, true);
    state.delivered("1", 1, old);
    check(state.size() == 8, "stale assignment cannot recover source");
    state.delivered("1", 9, state.ticket("1",9));
    check(state.size() == 7, "one success does not clear other sources");
    state.remove("2");
    check(state.size() == 6, "removal clears only that source");
    state.fail("unknown", 55, "SDK code 5", 105, true);
    state.fail("3", 3, "SDK code 5", 105, false);
    check(state.size() == 5, "unknown ignored; absent participant clears obsolete failure");
    dispatch_zoom_engine_failure("error", "connection_failed", false,
        [&](bool){ check(false, "fatal routed to media"); }, [&]{ ++fatal; });
    check(fatal == 1 && state.size() == 5, "independent fatal is never swallowed");
    for (int i=4; i<=8; ++i) state.remove(std::to_string(i));
    check(state.status(106).empty(), "all sources gone clears aggregate");
    state.assign("new", 10);
    check(state.fail("new",10,"SDK code 7",200,true), "new episode emits new notice");
    check(!state.terminal(201) && state.terminal(10200), "persistent single failure times out to actionable status");
    state.reset();
    check(state.status(10201).empty(), "meeting reset clears failures");
    state.assign("auto", 0);
    state.fail("auto", 42, "SDK code 5", 20000, true);
    state.delivered("auto", 41, state.ticket("auto",41));
    check(state.size() == 1, "dynamic source requires matching failure participant");
    state.delivered("auto", 42, state.ticket("auto",42));
    check(state.size() == 0, "dynamic source success clears failure");
    state.assign("stable", 22);
    state.fail("stable",22,"SDK code 5",21000,true);
    state.assign("stable",22);
    check(state.size() == 1, "retry subscribe is not recovery");
    state.delivered("stable",22,0);
    check(state.size() == 1, "unsuccessful/missing read ticket cannot recover");
    state.prune([](uint32_t){ return false; });
    check(state.size() == 0, "roster removal clears obsolete participant failure");
    for (const auto *kind : {"shm_create_failed", "subscribe_rejected", "shm_name_collision"})
        check(zoom_persistent_source_media_failure(kind), "known persistent media errors have dedicated route");
    check(!zoom_persistent_source_media_failure("meeting_failed"), "persistent media classifier excludes meeting failure");
    state.assign("sticky", 33);
    check(state.persistent_fail("sticky","audio memory failed",true), "first persistent media episode notice");
    check(!state.persistent_fail("sticky","audio memory failed",true), "persistent duplicates do not notify");
    state.delivered("sticky",33,state.ticket("sticky",33));
    check(state.terminal(22000), "video success never clears audio/shared memory failure");
    state.persistent_fail("unknown","ignored",false);
    state.remove("sticky");
    check(state.status(22000).empty(), "removal clears persistent source diagnostic, unknown excluded");
    state.persistent_fail("stopped","shared memory failure",true);
    state.stop();
    check(state.status(22000).empty(), "explicit stop retires persistent diagnostics");
    std::cout << "media-failure-state: all checks passed\n";
}
