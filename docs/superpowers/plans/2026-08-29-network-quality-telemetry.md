# Network-Quality Telemetry — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consume the SDK's per-user network-quality callbacks in the engine, keep a per-participant connection-quality table in the plugin, and surface per-source network health in the dock, the logs, and the control API — so an operator can tell "Zoom's link to that participant is dying" apart from "our pipeline is broken" without leaving OBS.

**Architecture:** The engine's existing `EngineMeetingEvent` (already registered as the `IMeetingServiceEvent` sink) fills in two callbacks it currently stubs out — `onUserNetworkStatusChanged` and `onMeetingStatisticsWarningNotification` — and forwards each as a line-JSON event over the existing E2P pipe. The plugin's `ZoomEngineClient::handle_event()` feeds them into a pure, header-only `NetworkQualityTable` (per-user, per-component, uplink/downlink split, staleness, warning latch, log rate-limiting — all decisions testable with no Qt/OBS/SDK). `ZoomOutputManager::outputs()` joins the table against each output's `participant_id` the same way `apply_output_health()` already joins the roster, which makes the fields flow to the dock and to `list_outputs`/`list_audio_sources` for free.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, named-pipe line-JSON IPC, Qt6 dock.

**Spec:** this document doubles as the spec. Requirements:

1. Every `onUserNetworkStatusChanged` / `onMeetingStatisticsWarningNotification` delivery reaches the plugin as an E2P event; neither is swallowed silently.
2. Per-participant quality state is keyed by meeting-scoped `userId`, split by uplink/downlink and component (audio/video/share), with staleness — a reading older than 60 s reads back Unknown, never its last value.
3. Transitions into/out of "bad" are logged with the participant's DISPLAY NAME (ids are meeting-scoped, never persisted), rate-limited so a flapping link cannot storm the log.
4. Per-source quality appears in the dock's signal cell (marker + tooltip) and in `list_outputs` / `list_audio_sources` for polling.
5. The meeting-wide statistics warning is latched, cleared on `Statistics_Warning_None` or after a hold window, and annotates the ISO encoder-demotion log line as context.
6. **Non-goals, binding:** telemetry INFORMS; it never ACTS. It must not trigger encoder demotion (`iso_demote_encoder()` stays startup-failure-driven — see the CLAUDE.md demotion-chain invariant), and it must not trigger any resubscribe. A quality-driven per-source resubscribe is explicitly DEFERRED: Zoom's transport already renegotiates on a degrading link (that is what `last_quality_stage`/`subscription_downgraded` record), so a plugin-side resubscribe on a dip tears down an adaptation mid-flight and pays full renegotiation to land in the same place — the resubscribe-storm shape live-caught 2026-08-19, self-inflicted. If a future defect proves a helpful case, it goes through the per-source path in `src/shm-resubscribe.h`'s world with its own rate limit and its own plan; `resubscribe_all()` stays engine-crash-recovery-only regardless.

## Global Constraints

- Build: `cmake --build build --config Release --parallel 8`. Test: `cd build && ctest -C Release --output-on-failure` — must be N/N green (63 tests today; 65 when this plan lands).
- Tests are plain executables, no framework, `check()`-style, one file per invariant cluster in `tests/`, registered in `CMakeLists.txt` with `add_executable` + `add_test`.
- Comments state the constraint the code cannot show; when a change is motivated by a live failure, say what happened, with numbers.
- Never call `ZoomOutputManager::resubscribe_all()` from any automatic path. Never run a second OBS instance while one is testing.
- SDK names verified against `third_party/zoom-sdk/h/meeting_service_interface.h`: `enum ConnectionQuality` (line 477: `Conn_Quality_Unknown`, `Conn_Quality_Very_Bad`, `Conn_Quality_Bad`, `Conn_Quality_Not_Good`, `Conn_Quality_Normal`, `Conn_Quality_Good`, `Conn_Quality_Excellent`), `enum MeetingComponentType` (line 499: `MeetingComponentType_Def = 0`, `_AUDIO`, `_VIDEO`, `_SHARE`), `enum StatisticsWarningType` (line 794: `Statistics_Warning_None`, `Statistics_Warning_Network_Quality_Bad`, `Statistics_Warning_Busy_System`). Both callbacks live on `IMeetingServiceEvent` itself (`onMeetingStatisticsWarningNotification(StatisticsWarningType)` at line 855, `onUserNetworkStatusChanged(MeetingComponentType, ConnectionQuality, unsigned int, bool)` at line 895) — no secondary event sink to register; `EngineMeetingEvent` (`engine/src/main.cpp:739`) already overrides both as empty stubs (`engine/src/main.cpp:1118`, `:1124`), so no new `SetEvent` wiring is needed anywhere.
- Direction semantics, stated once: the SDK's `uplink=true` means **that user's** uplink toward Zoom. For a remote participant feeding one of our sources, THEIR uplink is what degrades the media WE receive — so the dock marker keys on the worst of both directions rather than making the operator know that.

---

### Task 1: The pure quality table — `src/network-quality.h`

Every decision that could be wrong in an interesting way — staleness, the bad/ok boundary, log rate-limiting, the warning latch — goes into one Qt/OBS/SDK-free header, in the `src/talkback-plan.h` pattern, pinned by a host test with no engine and no meeting. The header mirrors the three SDK enums as plain `int` constants instead of including SDK headers: the plugin must compile without the SDK and the wire carries integers anyway.

**Files:**
- Create: `src/network-quality.h`
- Create: `tests/network-quality-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoTalkbackDockStateTest` block, `CMakeLists.txt:1136-1143`)

**Interfaces:**
- Consumes: nothing.
- Produces: `class NetworkQualityTable` with `NetQualityUpdate update(uint32_t user_id, int component, int quality, bool uplink, uint64_t now_ms)`, `int worst(uint32_t user_id, bool uplink, uint64_t now_ms) const`, `void note_stats_warning(int type, uint64_t now_ms)`, `bool stats_warning_active(uint64_t now_ms) const`, `void clear()`; free helpers `const char *network_quality_name(int q)`, `bool network_quality_is_bad(int q)`; constants `kNetQuality*`, `kNetComponent*`, `kStatsWarning*`, `kNetQualityStaleMs`, `kNetLogMinIntervalMs`, `kStatsWarningHoldMs`. Tasks 2–5 consume these.

- [ ] **Step 1: Write the failing test**

Create `tests/network-quality-test.cpp`:

```cpp
// tests/network-quality-test.cpp
// The per-participant network-quality table. Staleness, the bad/ok
// boundary, and log rate-limiting are what a flapping link exercises
// hardest, and none are visible in any integration path.
#include "network-quality.h"

#include <iostream>
#include <string>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

int main()
{
    NetworkQualityTable t;

    check(t.worst(42, true, 1000) == kNetQualityUnknown,
          "a never-seen user did not read back Unknown");

    // worst() is the minimum across components, per direction.
    t.update(42, kNetComponentVideo, kNetQualityGood, true, 1000);
    t.update(42, kNetComponentAudio, kNetQualityBad, true, 1000);
    t.update(42, kNetComponentVideo, kNetQualityExcellent, false, 1000);
    check(t.worst(42, true, 1000) == kNetQualityBad,
          "uplink worst did not pick the bad audio cell");
    check(t.worst(42, false, 1000) == kNetQualityExcellent,
          "downlink read a cell only ever reported uplink");

    // Staleness: an old reading is Unknown, never its last value.
    check(t.worst(42, true, 1000 + kNetQualityStaleMs + 1) == kNetQualityUnknown,
          "a reading older than kNetQualityStaleMs still read back Bad");

    // Unknown reports never mask a real reading.
    t.update(7, kNetComponentVideo, kNetQualityNormal, true, 2000);
    t.update(7, kNetComponentAudio, kNetQualityUnknown, true, 2000);
    check(t.worst(7, true, 2000) == kNetQualityNormal,
          "an Unknown cell was treated as worse than a real reading");

    // The bad boundary: Very_Bad and Bad only.
    check(network_quality_is_bad(kNetQualityVeryBad), "Very_Bad not bad");
    check(network_quality_is_bad(kNetQualityBad), "Bad not bad");
    check(!network_quality_is_bad(kNetQualityNotGood), "Not_Good graded bad");
    check(!network_quality_is_bad(kNetQualityUnknown), "Unknown graded bad");

    // Log gating: entering bad always logs; wobbles are rate-limited;
    // a fresh re-entry into bad is NOT (bad news is never delayed).
    NetworkQualityTable g;
    check(g.update(9, kNetComponentVideo, kNetQualityVeryBad, true, 5000).should_log,
          "the transition INTO bad was not flagged for logging");
    check(!g.update(9, kNetComponentVideo, kNetQualityBad, true, 5100).should_log,
          "a same-side wobble 100ms later logged again (storm shape)");
    check(!g.update(9, kNetComponentVideo, kNetQualityNormal, true, 5200).should_log,
          "recovery 200ms after the bad log was not rate-limited");
    check(g.update(9, kNetComponentVideo, kNetQualityVeryBad, true, 5300).should_log,
          "a re-entry into bad was rate-limited away");
    check(g.update(9, kNetComponentVideo, kNetQualityNormal, true,
                   5300 + kNetLogMinIntervalMs + 1).should_log,
          "recovery after the window did not log its all-clear");

    // Warning latch: held, expired by the hold window, cleared by None.
    t.note_stats_warning(kStatsWarningNetworkQualityBad, 10000);
    check(t.stats_warning_active(10000 + kStatsWarningHoldMs - 1),
          "the warning did not hold inside the window");
    check(!t.stats_warning_active(10000 + kStatsWarningHoldMs + 1),
          "the warning outlived its hold window with no re-report");
    t.note_stats_warning(kStatsWarningNetworkQualityBad, 20000);
    t.note_stats_warning(kStatsWarningNone, 20001);
    check(!t.stats_warning_active(20002),
          "Statistics_Warning_None did not clear the latch immediately");

    // clear(): ids are meeting-scoped; a new meeting starts from nothing.
    t.clear();
    check(t.worst(42, true, 1000) == kNetQualityUnknown,
          "clear() left a previous meeting's user id readable");
    check(!t.stats_warning_active(20000), "clear() left the warning latched");

    // Names for the wire and the tooltip; out-of-range degrades to unknown.
    check(std::string(network_quality_name(kNetQualityVeryBad)) == "very_bad",
          "name mapping wrong for Very_Bad");
    check(std::string(network_quality_name(99)) == "unknown",
          "an out-of-range quality did not degrade to unknown");

    if (failures == 0)
        std::cout << "network-quality: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Add to `CMakeLists.txt` immediately after the `add_test(NAME CoreVideoTalkbackDockState ...)` block (`CMakeLists.txt:1142-1143`):

```cmake
    # The per-participant network-quality table. Pure and header-only so the
    # staleness, bad-boundary, rate-limit and warning-latch decisions are
    # pinned with no engine and no meeting. See src/network-quality.h.
    add_executable(CoreVideoNetworkQualityTest
        tests/network-quality-test.cpp
    )
    target_include_directories(CoreVideoNetworkQualityTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoNetworkQuality
             COMMAND CoreVideoNetworkQualityTest)
```

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoNetworkQualityTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'network-quality.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/network-quality.h`:

```cpp
#pragma once
//
// network-quality.h — per-participant connection-quality state.
//
// Exists so that when a source goes stale or an ISO encoder dies, the
// operator can tell "Zoom's link to that participant is dying" apart from
// "our pipeline is broken" — the 2026-08-19 stale-recovery incident burned
// hours on exactly that ambiguity.
//
// TELEMETRY INFORMS, IT NEVER ACTS. Nothing here may drive encoder
// demotion (startup-failure-driven by invariant) or any resubscribe
// (Zoom's transport already adapts; resubscribing on a dip tears the
// adaptation down mid-flight — the 2026-08-19 resubscribe-storm shape).
//
// SDK enums mirrored as plain ints, values pinned to
// meeting_service_interface.h (ConnectionQuality :477,
// MeetingComponentType :499, StatisticsWarningType :794): this header must
// compile plugin-side with no SDK, and the pipe carries integers anyway.
// Keyed by meeting-scoped userId; cleared at every meeting boundary; never
// persisted — display names are joined in only at render/log time.
//
// Free of Qt / OBS / Zoom SDK so every decision is pinned by
// tests/network-quality-test.cpp.
//
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// ConnectionQuality, meeting_service_interface.h:477.
constexpr int kNetQualityUnknown   = 0; // Conn_Quality_Unknown
constexpr int kNetQualityVeryBad   = 1; // Conn_Quality_Very_Bad
constexpr int kNetQualityBad       = 2; // Conn_Quality_Bad
constexpr int kNetQualityNotGood   = 3; // Conn_Quality_Not_Good
constexpr int kNetQualityNormal    = 4; // Conn_Quality_Normal
constexpr int kNetQualityGood      = 5; // Conn_Quality_Good
constexpr int kNetQualityExcellent = 6; // Conn_Quality_Excellent

// MeetingComponentType, meeting_service_interface.h:499.
constexpr int kNetComponentDef   = 0;
constexpr int kNetComponentAudio = 1;
constexpr int kNetComponentVideo = 2;
constexpr int kNetComponentShare = 3;
constexpr int kNetComponentCount = 4;

// StatisticsWarningType, meeting_service_interface.h:794.
constexpr int kStatsWarningNone              = 0;
constexpr int kStatsWarningNetworkQualityBad = 1;
constexpr int kStatsWarningBusySystem        = 2;

// A reading Zoom has not refreshed in this long reads back Unknown. The SDK
// fires on CHANGE, so a stable link goes quiet — but so does a participant
// who LEFT, and their last reading must not describe whoever recycles the
// id. The roster join at render time is the identity guard; this is the
// backstop.
constexpr uint64_t kNetQualityStaleMs = 60000;

// Per-user log floor. Entering bad ALWAYS logs; everything else is
// suppressed inside this window so a link flapping at callback rate cannot
// storm the log — the message-storm shape this codebase has a live
// incident about.
constexpr uint64_t kNetLogMinIntervalMs = 5000;

// How long a statistics warning stays latched with no further report.
// Statistics_Warning_None clears immediately; the window is the backstop
// for when Zoom never sends one.
constexpr uint64_t kStatsWarningHoldMs = 30000;

inline const char *network_quality_name(int q)
{
    switch (q) {
    case kNetQualityVeryBad:   return "very_bad";
    case kNetQualityBad:       return "bad";
    case kNetQualityNotGood:   return "not_good";
    case kNetQualityNormal:    return "normal";
    case kNetQualityGood:      return "good";
    case kNetQualityExcellent: return "excellent";
    default:                   return "unknown";
    }
}

// The operator-facing boundary. Not_Good is deliberately NOT bad: Zoom
// reports it routinely on links delivering flawless 720p, and a marker
// that cries wolf trains the operator to ignore the marker.
inline bool network_quality_is_bad(int q)
{
    return q == kNetQualityVeryBad || q == kNetQualityBad;
}

struct NetQualityUpdate {
    bool changed = false;
    bool should_log = false; // one rate-limited log line owed for this
    int  previous = kNetQualityUnknown;
};

class NetworkQualityTable {
public:
    NetQualityUpdate update(uint32_t user_id, int component, int quality,
                            bool uplink, uint64_t now_ms)
    {
        NetQualityUpdate r;
        if (component < 0 || component >= kNetComponentCount ||
            quality < kNetQualityUnknown || quality > kNetQualityExcellent)
            return r; // unrecognised wire values update nothing
        Entry &e = m_users[user_id];
        Cell &c = e.cells[component][uplink ? 0 : 1];
        r.previous = c.quality;
        r.changed = c.quality != quality;
        const bool was_bad = network_quality_is_bad(c.quality);
        const bool is_bad = network_quality_is_bad(quality);
        c.quality = quality;
        c.updated_ms = now_ms;
        if (!r.changed)
            return r;
        if (is_bad && !was_bad) {
            r.should_log = true; // entering bad: always, immediately
            e.last_log_ms = now_ms;
        } else if (was_bad != is_bad &&
                   now_ms - e.last_log_ms >= kNetLogMinIntervalMs) {
            r.should_log = true; // recovery: rate-limited
            e.last_log_ms = now_ms;
        }
        return r;
    }

    // Worst non-stale, non-Unknown reading across components for one
    // direction; kNetQualityUnknown when nothing usable is known.
    int worst(uint32_t user_id, bool uplink, uint64_t now_ms) const
    {
        const auto it = m_users.find(user_id);
        if (it == m_users.end())
            return kNetQualityUnknown;
        int w = kNetQualityUnknown;
        for (int comp = 0; comp < kNetComponentCount; ++comp) {
            const Cell &c = it->second.cells[comp][uplink ? 0 : 1];
            if (c.quality == kNetQualityUnknown)
                continue;
            if (now_ms - c.updated_ms > kNetQualityStaleMs)
                continue;
            if (w == kNetQualityUnknown || c.quality < w)
                w = c.quality;
        }
        return w;
    }

    void note_stats_warning(int type, uint64_t now_ms)
    {
        m_warning_type = type;
        m_warning_ms = now_ms;
    }

    bool stats_warning_active(uint64_t now_ms) const
    {
        if (m_warning_type == kStatsWarningNone)
            return false;
        return now_ms - m_warning_ms <= kStatsWarningHoldMs;
    }

    int last_stats_warning() const { return m_warning_type; }

    // Meeting boundary: ids are meeting-scoped, so every reading and the
    // latch die with the meeting.
    void clear()
    {
        m_users.clear();
        m_warning_type = kStatsWarningNone;
        m_warning_ms = 0;
    }

    std::size_t user_count() const { return m_users.size(); }

private:
    struct Cell {
        int quality = kNetQualityUnknown;
        uint64_t updated_ms = 0;
    };
    struct Entry {
        Cell cells[kNetComponentCount][2]; // [component][0=up,1=down]
        uint64_t last_log_ms = 0;
    };
    std::unordered_map<uint32_t, Entry> m_users;
    int m_warning_type = kStatsWarningNone;
    uint64_t m_warning_ms = 0;
};
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --target CoreVideoNetworkQualityTest --parallel 8
cd build && ctest -C Release -R CoreVideoNetworkQuality --output-on-failure
```

Expected: PASS, `network-quality: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/network-quality.h tests/network-quality-test.cpp CMakeLists.txt
git commit -m "feat(net-telemetry): pure per-participant network-quality table"
```

---

### Task 2: Engine — forward the two callbacks over E2P

Both callbacks are already overridden as empty stubs on `EngineMeetingEvent` (`engine/src/main.cpp:1118`, `:1124`), which is registered as the `IMeetingServiceEvent` sink — so this is filling two bodies, not wiring an event interface. They ride the pipe as control events, ordered on the plugin's reader thread per the media-dispatch-lanes invariant. The SDK fires on change, but "on change" from a flapping link is still a storm and pipe storms have a live incident here — so identical consecutive values per (user, component, direction) are deduped engine-side before they cost a pipe write.

**Files:**
- Modify: `engine/src/main.cpp:1118` and `:1124-1126` (the two stubs), plus one member map on `EngineMeetingEvent` and one `.clear()` in the `MEETING_STATUS_DISCONNECTING`/`ENDED` branch (beside the existing `m_participants->detach()`, `engine/src/main.cpp:1058` region).

**Interfaces:**
- Consumes: `EngineIpc::write` (`engine/src/engine-writer.h`).
- Produces: E2P events `{"cmd":"network_status","component":<int>,"quality":<int>,"participant_id":<uint>,"uplink":<bool>}` and `{"cmd":"stats_warning","type":<int>}` — raw SDK enum values, which Task 1's constants mirror. Task 3 parses both.

- [ ] **Step 1: Write the failing check**

No unit test — SDK-callback-bound code in `main.cpp`, the same posture as the talkback plan's Task 3: the compile is the check, Task 3's dispatch test pins the exact wire shape emitted here (its test JSON is copied from this task's format strings), and Task 5 Step 4 proves the path live.

- [ ] **Step 2: Implement**

Replace the stub at `engine/src/main.cpp:1118`:

```cpp
    void onMeetingStatisticsWarningNotification(ZOOMSDK::StatisticsWarningType type) override
    {
        // Meeting-wide, not per-user. Forwarded raw; the plugin latches it
        // (kStatsWarningHoldMs) because Zoom does not reliably send
        // Statistics_Warning_None to clear it.
        EngineIpc::write(R"({"cmd":"stats_warning","type":)" +
                         std::to_string(static_cast<int>(type)) + "}");
    }
```

and the stub at `:1124`:

```cpp
    void onUserNetworkStatusChanged(ZOOMSDK::MeetingComponentType type,
                                    ZOOMSDK::ConnectionQuality level,
                                    unsigned int userId, bool uplink) override
    {
        // Dedupe identical consecutive values per (user, component,
        // direction): a link flapping between adjacent levels re-reports at
        // callback rate, and pipe storms have a live incident here. State,
        // not history, is what the plugin needs.
        const uint64_t key = (static_cast<uint64_t>(userId) << 3) |
                             (static_cast<uint64_t>(type) << 1) |
                             (uplink ? 1u : 0u);
        const auto it = m_net_last.find(key);
        if (it != m_net_last.end() && it->second == static_cast<int>(level))
            return;
        m_net_last[key] = static_cast<int>(level);
        EngineIpc::write(
            R"({"cmd":"network_status","component":)" +
            std::to_string(static_cast<int>(type)) +
            R"(,"quality":)" + std::to_string(static_cast<int>(level)) +
            R"(,"participant_id":)" + std::to_string(userId) +
            R"(,"uplink":)" + (uplink ? "true" : "false") + "}");
    }
```

Add the member beside the other `EngineMeetingEvent` state (`m_raw_media_active` etc.), with `#include <unordered_map>` if the build asks for it:

```cpp
    // Last forwarded quality per (user<<3 | component<<1 | uplink). Cleared
    // at the meeting boundary: ids are meeting-scoped, and a stale entry
    // would suppress the first report for whoever recycles the id.
    std::unordered_map<uint64_t, int> m_net_last;
```

In `onMeetingStatusChanged`'s `MEETING_STATUS_DISCONNECTING`/`MEETING_STATUS_ENDED` branch, beside `if (m_participants) m_participants->detach();`:

```cpp
            m_net_last.clear();
```

- [ ] **Step 3: Build and run the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: engine links clean; 64/64 green (63 existing plus `CoreVideoNetworkQuality`).

- [ ] **Step 4: Commit**

```sh
git add engine/src/main.cpp
git commit -m "feat(net-telemetry): forward SDK network-status and stats-warning events over E2P"
```

---

### Task 3: Plugin — dispatch into the table, log transitions by name

The talkback feature shipped two Majors in WIRING no header-only test could reach (`handle_event`'s report-shape-to-transition mapping); the fix was a Qt-JSON-only dispatch header (`src/talkback-nomination-dispatch.h`). Same lesson applied from the start: the event-shape-to-table mapping goes into `src/network-quality-dispatch.h`, driven by a test with the exact JSON Task 2 emits. `handle_event()` then only locks, delegates, and logs — and the log line joins the roster to print a display name, because a log full of meeting-scoped ids identifies nobody after the meeting ends.

**Files:**
- Create: `src/network-quality-dispatch.h`, `tests/network-quality-dispatch-test.cpp`
- Modify: `src/zoom-engine-client.h` — accessor beside `roster()` (`:268`), member beside `m_roster` (`:388`)
- Modify: `src/zoom-engine-client.cpp` — two branches in `handle_event()` (insert after the `active_speaker` branch, `:1625-1636`, before the `frame`/`audio` gate at `:1638`); world-reset in the `"left"` branch (`:1453`)
- Modify: `CMakeLists.txt` (register after Task 1's block)

**Interfaces:**
- Consumes: `NetworkQualityTable` (Task 1); wire shapes (Task 2).
- Produces: `NetworkEventResult network_quality_apply_event(NetworkQualityTable &table, const QString &cmd, const QJsonObject &obj, uint64_t now_ms)`; `NetworkQualityTable ZoomEngineClient::network_quality_table() const` — a by-value snapshot under `m_mtx` (the table is a few dozen small entries; a copy means no caller carries `m_mtx` into Qt paint or JSON code). Tasks 4–5 consume the accessor.

- [ ] **Step 1: Write the failing test**

Create `tests/network-quality-dispatch-test.cpp` (Qt-JSON-only, the `tests/zoom-control-parse-test.cpp` bar):

```cpp
// tests/network-quality-dispatch-test.cpp
// The wire-shape-to-table mapping, pinned separately from the table because
// both talkback Majors lived in exactly this seam: correct state machines
// wired to the wrong report shape.
#include "network-quality-dispatch.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
static QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

int main()
{
    NetworkQualityTable t;

    // The exact shape engine/src/main.cpp emits routes into the table.
    const NetworkEventResult r1 = network_quality_apply_event(
        t, "network_status",
        parse(R"({"cmd":"network_status","component":2,"quality":1,)"
              R"("participant_id":123,"uplink":true})"),
        1000);
    check(r1.handled, "network_status was not handled");
    check(r1.user_id == 123 && r1.quality == kNetQualityVeryBad && r1.uplink,
          "network_status fields did not decode");
    check(r1.should_log, "the first drop into Very_Bad was not flagged to log");
    check(t.worst(123, true, 1000) == kNetQualityVeryBad,
          "the table did not record the update");

    // stats_warning latches.
    const NetworkEventResult r2 = network_quality_apply_event(
        t, "stats_warning", parse(R"({"cmd":"stats_warning","type":1})"), 2000);
    check(r2.handled && r2.is_warning &&
              r2.warning_type == kStatsWarningNetworkQualityBad,
          "stats_warning did not decode");
    check(t.stats_warning_active(2000), "the warning did not latch");

    // Unrelated commands pass through untouched.
    check(!network_quality_apply_event(
              t, "participants", parse(R"({"cmd":"participants"})"), 3000).handled,
          "an unrelated cmd was claimed by the network dispatcher");

    // Missing fields degrade to no-op, never to a bogus user-0 entry.
    const NetworkEventResult r4 = network_quality_apply_event(
        t, "network_status", parse(R"({"cmd":"network_status"})"), 4000);
    check(r4.handled && !r4.should_log,
          "a field-less network_status line asked to be logged");
    check(t.worst(0, true, 4000) == kNetQualityUnknown,
          "a field-less line created a user-0 reading");

    if (failures == 0)
        std::cout << "network-quality-dispatch: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

Register in `CMakeLists.txt` after Task 1's block, copying the `Qt6::Core` link line from the `tests/zoom-control-parse-test.cpp` block (the existing Qt-JSON-only precedent):

```cmake
    # The E2P-event-to-table mapping. Separate from the table test because
    # both talkback Majors lived in this seam. Qt JSON only, no OBS.
    add_executable(CoreVideoNetworkQualityDispatchTest
        tests/network-quality-dispatch-test.cpp
    )
    target_include_directories(CoreVideoNetworkQualityDispatchTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoNetworkQualityDispatchTest PRIVATE Qt6::Core)
    add_test(NAME CoreVideoNetworkQualityDispatch
             COMMAND CoreVideoNetworkQualityDispatchTest)
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoNetworkQualityDispatchTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'network-quality-dispatch.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/network-quality-dispatch.h`:

```cpp
#pragma once
//
// network-quality-dispatch.h — maps E2P network events onto the table.
// Qt-JSON-only, no OBS/socket/thread dependency, in the pattern of
// talkback-nomination-dispatch.h and for the same reason: wiring bugs
// inlined in handle_event() are unreachable by host tests.
//
#include "network-quality.h"

#include <QJsonObject>
#include <QString>

struct NetworkEventResult {
    bool handled = false;
    bool should_log = false; // the table's rate-limited verdict
    bool is_warning = false;
    uint32_t user_id = 0;
    int component = kNetComponentDef;
    int quality = kNetQualityUnknown;
    bool uplink = false;
    int warning_type = kStatsWarningNone;
    int previous = kNetQualityUnknown;
};

inline NetworkEventResult network_quality_apply_event(NetworkQualityTable &table,
                                                      const QString &cmd,
                                                      const QJsonObject &obj,
                                                      uint64_t now_ms)
{
    NetworkEventResult r;
    if (cmd == QStringLiteral("network_status")) {
        r.handled = true;
        r.user_id = static_cast<uint32_t>(obj.value("participant_id").toInt(0));
        if (r.user_id == 0)
            return r; // a garbled line must not mint a user-0 entry
        r.component = obj.value("component").toInt(kNetComponentDef);
        r.quality = obj.value("quality").toInt(kNetQualityUnknown);
        r.uplink = obj.value("uplink").toBool(false);
        const NetQualityUpdate u =
            table.update(r.user_id, r.component, r.quality, r.uplink, now_ms);
        r.should_log = u.should_log;
        r.previous = u.previous;
        return r;
    }
    if (cmd == QStringLiteral("stats_warning")) {
        r.handled = true;
        r.is_warning = true;
        r.warning_type = obj.value("type").toInt(kStatsWarningNone);
        table.note_stats_warning(r.warning_type, now_ms);
        // Warnings are meeting-wide and rare; the caller logs every one.
        r.should_log = r.warning_type != kStatsWarningNone;
        return r;
    }
    return r;
}
```

In `src/zoom-engine-client.h` (add `#include "network-quality.h"`), declare beside `roster()` (`:268`) and add the member beside `m_roster` (`:388`):

```cpp
    // Snapshot by value: the table is small, and a copy means no caller
    // carries m_mtx into paint/JSON code.
    NetworkQualityTable network_quality_table() const;
```

```cpp
    NetworkQualityTable m_network_quality;
```

In `src/zoom-engine-client.cpp` (add `#include "network-quality-dispatch.h"`), insert after the `active_speaker` branch (`:1636`), before the `if (cmd != "frame" && cmd != "audio") return;` gate:

```cpp
    if (cmd == "network_status" || cmd == "stats_warning") {
        // Control event, reader thread, ordered — deliberately NOT a media
        // lane: rare and cheap, and ordering against "participants"/"left"
        // is what keeps the table's ids meaningful.
        const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
        NetworkEventResult r;
        std::string who;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            r = network_quality_apply_event(m_network_quality, cmd, obj, now_ms);
            for (const auto &p : m_roster)
                if (p.user_id == r.user_id) { who = p.display_name; break; }
        }
        if (r.should_log && r.is_warning) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Zoom meeting statistics warning: %s",
                 r.warning_type == kStatsWarningNetworkQualityBad
                     ? "network quality bad" : "system busy");
        } else if (r.should_log) {
            // Named, never id-only: ids are meeting-scoped and identify
            // nobody once the log is read after the meeting.
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Network quality for %s (id %u): %s -> %s "
                 "(%s, %s)",
                 who.empty() ? "<not in roster>" : who.c_str(), r.user_id,
                 network_quality_name(r.previous),
                 network_quality_name(r.quality),
                 r.component == kNetComponentAudio ? "audio"
                 : r.component == kNetComponentVideo ? "video"
                 : r.component == kNetComponentShare ? "share" : "default",
                 r.uplink ? "their uplink" : "their downlink");
        }
        return;
    }
```

In the `"left"` branch (`:1453`), inside the same `m_mtx` scope its other world-resets use:

```cpp
        m_network_quality.clear(); // ids are meeting-scoped; see network-quality.h
```

Implement the accessor beside `roster()`'s definition:

```cpp
NetworkQualityTable ZoomEngineClient::network_quality_table() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_network_quality;
}
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: 65/65 green, `network-quality-dispatch: all tests passed` among them.

- [ ] **Step 5: Commit**

```sh
git add src/network-quality-dispatch.h src/zoom-engine-client.h src/zoom-engine-client.cpp tests/network-quality-dispatch-test.cpp CMakeLists.txt
git commit -m "feat(net-telemetry): dispatch network events into the table and log transitions by name"
```

---

### Task 4: Join quality onto per-source info and the control API

`ZoomOutputManager::outputs()` already runs every snapshot through `apply_output_health(out, roster, raw_media_active)` (`src/zoom-output-manager.cpp:174`); network quality joins the identical way, keyed by `participant_id`, as a sibling function in the same header so the same test file pins it. The fields are NEW fields, never a new `ZoomOutputHealthReason`: health reasons gate recovery actions and dock states, and telemetry informs, it never acts — a `ParticipantVideoOff` must not be displaced by "their network is bad".

**Files:**
- Modify: `src/zoom-output-manager.h` — two fields on `ZoomOutputInfo` (after `last_quality_stage`, `:55`)
- Modify: `src/zoom-output-health.h` — `apply_network_quality()` beside `apply_output_health()` (`:8`)
- Modify: `src/zoom-output-manager.cpp:174-176` — call it in `outputs()`
- Modify: `src/zoom-control-server.cpp` — fields in `output_to_json` (`:187`) and the `list_audio_sources` loop (`:598-620`)
- Test: `tests/output-health-test.cpp` (extend — same invariant cluster: per-source health grading from a snapshot)

**Interfaces:**
- Consumes: `NetworkQualityTable`, `network_quality_name` (Task 1); `ZoomEngineClient::network_quality_table()` (Task 3).
- Produces: `ZoomOutputInfo::net_quality_uplink` / `net_quality_downlink` (`int`, `kNetQualityUnknown` when unknown); `void apply_network_quality(std::vector<ZoomOutputInfo> &outputs, const NetworkQualityTable &net, uint64_t now_ms)`; JSON fields `network_uplink` / `network_downlink` (quality-name string, or null when unreported — the `av_offset_us` null convention) on both list responses. Task 5 reads the struct fields.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/output-health-test.cpp` before its final pass/fail block (reuse its existing `check` helper; add `#include "network-quality.h"`):

```cpp
    // --- Network quality joins by participant_id and never touches
    //     health_reason: telemetry informs, it never acts ---
    {
        NetworkQualityTable net;
        net.update(77, kNetComponentVideo, kNetQualityVeryBad, true, 5000);
        std::vector<ZoomOutputInfo> outs(2);
        outs[0].participant_id = 77;
        outs[0].health_reason = ZoomOutputHealthReason::ParticipantVideoOff;
        outs[1].participant_id = 0; // unassigned: must stay unknown
        apply_network_quality(outs, net, 5000);
        check(outs[0].net_quality_uplink == kNetQualityVeryBad,
              "uplink quality did not join onto the assigned output");
        check(outs[0].net_quality_downlink == kNetQualityUnknown,
              "a direction never reported read back non-unknown");
        check(outs[0].health_reason == ZoomOutputHealthReason::ParticipantVideoOff,
              "apply_network_quality displaced an existing health reason");
        check(outs[1].net_quality_uplink == kNetQualityUnknown,
              "an unassigned output (participant_id 0) got a reading");
        apply_network_quality(outs, net, 5000 + kNetQualityStaleMs + 1);
        check(outs[0].net_quality_uplink == kNetQualityUnknown,
              "a stale reading survived the join");
    }
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build --config Release --target CoreVideoOutputHealthTest --parallel 8
```

Expected: FAIL to compile — `'net_quality_uplink': is not a member` / `'apply_network_quality': identifier not found`. (If the executable target is named differently, take the name from the `tests/output-health-test.cpp` block in `CMakeLists.txt` — the test file, not the target name, is the anchor.)

- [ ] **Step 3: Write minimal implementation**

In `src/zoom-output-manager.h`, after `std::string last_quality_stage;` (`:55`):

```cpp
    // Zoom's own report of this participant's link, joined in by
    // apply_network_quality() (src/zoom-output-health.h). kNetQualityUnknown
    // when unassigned, unreported, or stale. INFORMATIONAL ONLY: nothing
    // may branch a demotion or resubscribe on these — see network-quality.h.
    int net_quality_uplink = 0;   // kNetQualityUnknown
    int net_quality_downlink = 0; // kNetQualityUnknown
```

In `src/zoom-output-health.h`, add `#include "network-quality.h"` and, after `apply_output_health`:

```cpp
// Joins Zoom's per-participant link quality onto the snapshot, keyed by
// participant_id like apply_output_health's roster join. Writes ONLY the
// two net_quality_* fields — a health_reason gates recovery actions, and
// "their network is bad" must inform, not displace ParticipantVideoOff.
inline void apply_network_quality(std::vector<ZoomOutputInfo> &outputs,
                                  const NetworkQualityTable &net,
                                  uint64_t now_ms)
{
    for (auto &out : outputs) {
        out.net_quality_uplink = kNetQualityUnknown;
        out.net_quality_downlink = kNetQualityUnknown;
        if (out.participant_id == 0)
            continue;
        // A remote participant's UPLINK is what degrades the media we
        // receive from them; both directions are surfaced so the dock can
        // key on the worst.
        out.net_quality_uplink = net.worst(out.participant_id, true, now_ms);
        out.net_quality_downlink = net.worst(out.participant_id, false, now_ms);
    }
}
```

In `src/zoom-output-manager.cpp`, after the `apply_output_health(...)` call (`:174-175`):

```cpp
    apply_network_quality(out, ZoomEngineClient::instance().network_quality_table(),
                          os_gettime_ns() / 1000000ULL);
```

In `src/zoom-control-server.cpp` (add `#include "network-quality.h"`), in `output_to_json` (`:187`) beside the existing quality fields:

```cpp
    // Null means NOT REPORTED (unknown/stale) — the av_offset_us convention.
    obj["network_uplink"] = o.net_quality_uplink == kNetQualityUnknown
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(QString::fromUtf8(network_quality_name(o.net_quality_uplink)));
    obj["network_downlink"] = o.net_quality_downlink == kNetQualityUnknown
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(QString::fromUtf8(network_quality_name(o.net_quality_downlink)));
```

In the `list_audio_sources` handler (`:598`) — a `CoreVideoAudioSourceInfo` is not a `ZoomOutputInfo`, so the join happens here — hoist one snapshot above the loop:

```cpp
        const NetworkQualityTable net =
            ZoomEngineClient::instance().network_quality_table();
        const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
```

and inside the loop, beside `obj["overrun_slots"]`:

```cpp
            const int up = net.worst(a.participant_id, true, now_ms);
            const int down = net.worst(a.participant_id, false, now_ms);
            obj["network_uplink"] = up == kNetQualityUnknown
                ? QJsonValue(QJsonValue::Null)
                : QJsonValue(QString::fromUtf8(network_quality_name(up)));
            obj["network_downlink"] = down == kNetQualityUnknown
                ? QJsonValue(QJsonValue::Null)
                : QJsonValue(QString::fromUtf8(network_quality_name(down)));
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: 65/65 green.

- [ ] **Step 5: Commit**

```sh
git add src/zoom-output-manager.h src/zoom-output-health.h src/zoom-output-manager.cpp src/zoom-control-server.cpp tests/output-health-test.cpp
git commit -m "feat(net-telemetry): join link quality onto outputs and the control API"
```

---

### Task 5: Dock surface, demotion-log context, live check

The operator surface: a marker in the dock's per-source signal cell when Zoom itself says the link is bad, a tooltip line naming which direction, and one annotation on the ISO demotion log line so a demotion during a meeting-wide network warning reads as "the box was fighting the network too", not just "the encoder died". Rides the existing 100 ms refresh (`m_refresh_timer`, `src/zoom-dock.cpp:1111-1117`) — `refresh_outputs()` already re-reads `ZoomOutputManager::instance().outputs()`, which Task 4 populated, so no new timer and no new data path.

**Files:**
- Modify: `src/zoom-dock.cpp` — `signal_label` (`:391`), `signal_tooltip` (`:430`), plus `#include "network-quality.h"`
- Modify: `src/zoom-iso-recorder.cpp` — the encoder-demotion `blog` (`:901-905`), plus includes
- Test: none new — the display strings are Qt formatting over fields Task 4's test pins, the same posture as every other `signal_label` branch; the demotion annotation is verified by Step 3's read-through and Step 4 live.

**Interfaces:**
- Consumes: `ZoomOutputInfo::net_quality_uplink`/`net_quality_downlink` (Task 4); `network_quality_is_bad`, `network_quality_name` (Task 1); `ZoomEngineClient::network_quality_table().stats_warning_active()` (Task 3).
- Produces: operator-visible strings only.

- [ ] **Step 1: Dock marker and tooltip**

In `signal_label` (`src/zoom-dock.cpp:391`), compute a suffix at the top:

```cpp
    // Zoom's own verdict on the participant's link. A suffix, never a
    // health_reason: it must coexist with (and explain) Waiting/Stale, not
    // replace them. Worst of both directions — their uplink is what
    // degrades what we receive, but the operator shouldn't need to know.
    const QString net_suffix =
        (network_quality_is_bad(output.net_quality_uplink) ||
         network_quality_is_bad(output.net_quality_downlink))
            ? QStringLiteral("\n! network")
            : QString();
```

and append `+ net_suffix` to the four live-signal returns in the function's tail — `Waiting`, `! Stale`, `%1%2x%3\n%4 fps`, and `%1%2x%3` (the health-reason early returns above them already name a dominating problem). E.g. the fps return becomes:

```cpp
        return QString("%1%2x%3\n%4 fps")
            .arg(prefix)
            .arg(output.observed_width)
            .arg(output.observed_height)
            .arg(output.observed_fps, 0, 'f', 1) + net_suffix;
```

In `signal_tooltip` (`:430`), build once at the top —

```cpp
    QString net_line;
    if (output.net_quality_uplink != kNetQualityUnknown ||
        output.net_quality_downlink != kNetQualityUnknown) {
        net_line = QString("\nZoom link quality for this participant: "
                           "uplink %1, downlink %2 (their side).")
            .arg(QString::fromUtf8(network_quality_name(output.net_quality_uplink)))
            .arg(QString::fromUtf8(network_quality_name(output.net_quality_downlink)));
    }
```

— and append `+ net_line` to EVERY return in the function, including the health-reason early returns: a tooltip has room, and "Participant video off" plus "uplink very_bad" together is precisely the diagnosis this feature exists to put in front of the operator.

- [ ] **Step 2: ISO demotion log context**

In `src/zoom-iso-recorder.cpp` at the demotion warning (`:901-905`), add `#include "network-quality.h"` and `#include "zoom-engine-client.h"` (if not already present) and extend the existing `blog` — the demotion DECISION is untouched, per the demotion-chain invariant; only the sentence changes (`now_ns` already exists in scope, `:894`):

```cpp
            const bool net_warn =
                ZoomEngineClient::instance().network_quality_table()
                    .stats_warning_active(now_ns / 1000000ULL);
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] ISO encoder %s failed at startup for "
                 "%s; retrying with %s%s",
                 session.video_encoder.c_str(),
                 session.source_name.c_str(), next.c_str(),
                 net_warn ? " (Zoom meeting network warning active)" : "");
```

- [ ] **Step 3: Build, full suite, self-read**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: 65/65 green. Re-read both dock functions end to end and confirm every return path carries its suffix/line — a missed return is silent and this step is the only unit-level check they get.

- [ ] **Step 4: Live check against a real meeting**

Not a hard gate (the interfaces are verified in the vendored headers and already registered), but the callbacks' real firing rate and value distribution are assumptions until watched once. Install the matched pair (both binaries, SHA256-verified — a DLL-only copy is this project's canonical mistake), join a test meeting with two participants, then:

```sh
printf '{"cmd":"list_outputs"}\n' | nc 127.0.0.1 19870
```

- `network_uplink`/`network_downlink` are null before any callback fires and become quality names after (throttle a participant's connection, or wait — real links report transitions within minutes).
- The log shows `Network quality for <name> (id N): ...` with display names, and a bad patch produces ONE line, not a stream.
- The dock cell shows `! network` for the degraded participant; the tooltip names the direction.
- Send `{"cmd":"leave"}` before closing OBS.

Record what fired and how often in `docs/superpowers/notes/2026-08-29-network-telemetry-live-notes.md` — the dedupe and rate-limit constants were chosen on assumptions this run confirms or corrects.

- [ ] **Step 5: Update CLAUDE.md and commit**

Add a short invariants entry to CLAUDE.md (telemetry informs / never acts; the fields; null-means-unreported) — docs-updated is part of done.

```sh
git add src/zoom-dock.cpp src/zoom-iso-recorder.cpp CLAUDE.md docs/superpowers/notes/2026-08-29-network-telemetry-live-notes.md
git commit -m "feat(net-telemetry): dock network markers and demotion-log context"
```

---

## Self-Review

**Placeholder scan:** none. Every code step contains the actual code; Task 2's "no unit test" and Task 5's "no new test" are explicit statements about SDK-bound and Qt-formatting code with named verification routes (Task 3's dispatch test pins the wire shape; Task 4's test pins the fields; Task 5 Step 4 is the live check), not deferred work.

**SDK ground truth:** both callbacks verified on `IMeetingServiceEvent` (`meeting_service_interface.h:855`, `:895`); all three enums quoted member-by-member from lines 477/499/794 and mirrored as pinned int constants in Task 1. No SDK interface this plan needs is missing from the vendored headers. (`GetVideoConnQuality`/`GetAudioConnQuality`/`GetSharingConnQuality`, `:1066-1085`, poll only OUR OWN connection and are deliberately unused — they cannot answer per-remote-participant questions.)

**Type consistency:** `NetworkQualityTable::update(uint32_t, int, int, bool, uint64_t)` (Task 1) is called by `network_quality_apply_event` (Task 3) with those types; `worst(uint32_t, bool, uint64_t)` is called in Tasks 1, 3 (test), and 4 (twice); `net_quality_uplink`/`net_quality_downlink` are `int` in Task 4's struct and read as `int` in Task 5; `network_quality_table()` returns `NetworkQualityTable` by value in Task 3 and is consumed by value in Tasks 4 and 5. The wire field `participant_id` (Task 2) matches the key Task 3 reads and the existing `DebugEvent.participant_id` naming.

**Invariant compliance:** no `resubscribe_all` anywhere; the demotion chain's guard and decision are untouched (Task 5 changes one format string); control events stay ordered on the reader thread; ids are meeting-scoped — cleared engine-side at the meeting boundary (Task 2), plugin-side in the `"left"` world-reset (Task 3), and never persisted or logged without a name join.
