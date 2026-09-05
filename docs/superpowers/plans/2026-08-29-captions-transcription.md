# Zoom Live Transcription — Captions Integration Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Subscribe to Zoom live transcription in the engine and deliver a per-speaker caption stream (speaker name + text + timestamps) to the plugin, where it drives an operator-named OBS text source, is pollable over the control API, and lands in a sidecar transcript log time-aligned to the ISO recorder's clock.

**Architecture:** The engine acquires `IClosedCaptionController` via `IMeetingService::GetMeetingClosedCaptionController()` (`third_party/zoom-sdk/h/meeting_service_interface.h:1234`), registers an `IClosedCaptionControllerEvent` sink, and forwards every `onLiveTranscriptionMsgInfoReceived` message as one `{"cmd":"caption",...}` line over the existing E2P pipe. Captions are control-rate (a few lines per second at most), so they ride the ordered control path on the pipe reader thread — the `src/media-event-queue.h` lanes are for media only and are not touched. The plugin coalesces partial/final messages per speaker in a pure header-only state machine, updates the caption text source, and — when ISO recording is active — appends finalized lines to one transcript file anchored to the same `os_gettime_ns()` clock every ISO session anchors to.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, named-pipe line-JSON IPC, Qt6.

**Spec:** There is no separate spec — this document doubles as the spec. Requirements:

- Engine can start and stop Zoom live transcription on command, and reports every gate it passes or falls off (support, permission, status) as its own stage line — surface gating, never swallow it.
- Every live transcription message reaches the plugin with speaker name, speaker id, text, operation type, and the SDK's timestamp — plus a plugin-side arrival timestamp on the ISO clock.
- Partial (`Add`/`Update`) vs final (`Complete`) messages are coalesced per speaker; `Delete` retracts.
- The operator names an OBS text source (any text source — lower-third scenes just reference it); the plugin keeps it showing the latest caption line as `Name: text`.
- `captions_status` on the control API returns transcription status and the latest line per speaker.
- While ISO recording is active, finalized lines are appended to `<output_dir>/transcript-<stamp>.txt`, offsets computed against a recording-start anchor on the same `os_gettime_ns()` clock the ISO sessions use.
- A repo-wide grep for `closedcaption|caption|transcri` confirms **no existing caption or transcription code exists in `src/` or `engine/src/`** — the only hits are site HTML, talkback docs, and an unrelated test comment. This is a greenfield feature; nothing is being extended or migrated.

**SDK ground truth and one gap, planned around:** everything needed exists in the tracked header `third_party/zoom-sdk/h/meeting_closedcaption_interface.h` — `IClosedCaptionController`, `IClosedCaptionControllerEvent`, `ILiveTranscriptionMessageInfo`, `SDKLiveTranscriptionStatus`, `SDKLiveTranscriptionOperationType`. Unlike talkback's `meeting_service_components/meeting_talkback_ctrl_interface.h` (which only the full-SDK drop supplies, hence the `EXISTS` gate at `CMakeLists.txt:1168`), this header lives in the tracked `h/` root, so **no new `EXISTS` gate is needed**; the engine target's existing SDK-present gate (`CMakeLists.txt:224`) covers it. The gap: `ILiveTranscriptionMessageInfo::GetTimeStamp()` returns `time_t` — whole seconds, epoch basis undocumented. That is useless for aligning to millisecond-accurate ISO recordings, so the wire carries it as `ts` for the record, but **all alignment uses the plugin's arrival `os_gettime_ns()`**, the exact clock `record_video_frame`/`record_audio_frame` timestamps already use. Caption latency (Zoom's ASR runs ~1–3 s behind speech) dominates either way; the transcript documents this in its header line.

## Global Constraints

- Build: `cmake --build build --config Release --parallel 8`. Test: `cd build && ctest -C Release --output-on-failure` — must be N/N green after every task.
- Tests are plain executables, no framework, `check()`-style, one file per invariant cluster in `tests/`, registered in `CMakeLists.txt` with `add_executable` + `add_test`.
- Comments state the constraint the code cannot show. When a change is motivated by a live failure, say what happened, with numbers.
- Participants are addressed by **display name**; never persist a Zoom user id — they are meeting-scoped and get recycled. The wire may carry `speaker_id` for correlation within one meeting, but every table the plugin keeps is keyed by name.
- Control events stay ordered on the pipe reader thread. Captions are control-rate: they take the ordered path in `ZoomEngineClient::handle_event`, never a media lane.
- SDK calls initiated by us (`StartLiveTranscription` etc.) run on the engine's command-loop thread, like every other command branch. SDK callbacks arrive on SDK threads and may only build strings and call the serialised `EngineIpc::write` (`engine/src/engine-writer.h`).
- Never run a second OBS instance while one is testing (pipe/SDK singleton collision, crash loop). Send `{"cmd":"leave"}` before closing OBS.
- Pure logic is header-only and Qt/OBS/SDK-free so plain-executable tests can pin it (repo pattern: `src/talkback-plan.h`, `src/talkback-nomination.h`).

---

### Task 1: Route the `captions_start` / `captions_stop` commands

The engine identifies commands by exact match on the declared `cmd` field (`src/engine-command.h`), never by substring — a substring dispatch once routed every `unsubscribe` into the `subscribe` branch. Two commands, not one with a boolean payload, matching the `start_media`/`stop_media` shape: the routing test can then pin each token independently.

**Files:**
- Modify: `src/engine-ipc.h:26` (add tokens after `IPC_CMD_TALKBACK_NOMINATE`)
- Modify: `src/engine-command.h:49` (enum, after `TalkbackNominate,`), `src/engine-command.h:104` (routing, before `return IpcCommand::Unknown;`)
- Test: `tests/engine-command-test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `IPC_CMD_CAPTIONS_START` (`"captions_start"`), `IPC_CMD_CAPTIONS_STOP` (`"captions_stop"`), `IpcCommand::CaptionsStart`, `IpcCommand::CaptionsStop`. Task 4 branches on the enums; Task 5 emits the literals.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/engine-command-test.cpp`, before the final `if (failures == 0)` block:

```cpp
    // --- Captions commands route exactly, and do not collide ---
    check(ipc_command_of(R"({"cmd":"captions_start"})") == IpcCommand::CaptionsStart,
          "captions_start did not route to IpcCommand::CaptionsStart");
    check(ipc_command_of(R"({"cmd":"captions_stop"})") == IpcCommand::CaptionsStop,
          "captions_stop did not route to IpcCommand::CaptionsStop");
    // A payload containing the token must not route (the substring disease).
    check(ipc_command_of(R"({"cmd":"join","display_name":"captions_start"})") ==
              IpcCommand::Join,
          "a payload containing 'captions_start' hijacked the join branch");
    check(ipc_command_of(R"({"cmd":"captions_startx"})") == IpcCommand::Unknown,
          "a longer command starting with captions_start matched it");
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
```

Expected: FAIL to compile with `'CaptionsStart' is not a member of 'IpcCommand'`.

- [ ] **Step 3: Write minimal implementation**

In `src/engine-ipc.h`, after the `IPC_CMD_TALKBACK_NOMINATE` line:

```c
#define IPC_CMD_CAPTIONS_START "captions_start"
#define IPC_CMD_CAPTIONS_STOP  "captions_stop"
```

In `src/engine-command.h`, add to the enum after `TalkbackNominate,`:

```cpp
    CaptionsStart,
    CaptionsStop,
```

and to `ipc_command_of`, before `return IpcCommand::Unknown;`:

```cpp
    if (cmd == IPC_CMD_CAPTIONS_START) return IpcCommand::CaptionsStart;
    if (cmd == IPC_CMD_CAPTIONS_STOP)  return IpcCommand::CaptionsStop;
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
cd build && ctest -C Release -R CoreVideoEngineCommand --output-on-failure
```

Expected: PASS, `engine-command: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/engine-ipc.h src/engine-command.h tests/engine-command-test.cpp
git commit -m "feat(captions): route the captions_start/captions_stop commands"
```

---

### Task 2: Caption coalescing state machine

Zoom does not send finished sentences. One utterance arrives as `Add`, then a burst of `Update`s rewriting the same message id, then `Complete` — and sometimes `Delete`, or no `Complete` at all before the speaker's next utterance starts. Something has to decide what "the current caption line" is and when a line is *final* enough for a transcript, and that decision must be pinnable without an engine or a meeting. This is the feature's `talkback-plan.h`: pure, header-only, no Qt/OBS/SDK.

**Files:**
- Create: `src/caption-stream.h`
- Test: `tests/caption-stream-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoTileRetry` block, `CMakeLists.txt:1320-1330`)

**Interfaces:**
- Consumes: nothing.
- Produces (all in `src/caption-stream.h`; Task 5 feeds `apply()` and reads `latest()`/`display_line()`, Task 6 consumes the returned finals):

```cpp
enum class CaptionOp { None = 0, Add = 1, Update = 2, Delete = 3, Complete = 4, NotSupported = 5 };
struct CaptionEvent { std::string msg_id; std::string speaker; uint32_t speaker_id;
                      std::string text; CaptionOp op; uint64_t arrival_ns; };
struct CaptionFinal { std::string speaker; std::string text; uint64_t first_seen_ns; };
class CaptionStream {
public:
    std::vector<CaptionFinal> apply(const CaptionEvent &ev);
    const std::map<std::string, std::string> &latest() const; // speaker -> line
    std::string display_line() const;                         // "Name: text" or ""
    void clear();
};
```

- [ ] **Step 1: Write the failing test**

Create `tests/caption-stream-test.cpp`:

```cpp
// tests/caption-stream-test.cpp
// Pins the coalescing rules for Zoom live transcription messages. The rule
// that earns its test: an utterance that never receives Complete must still
// reach the transcript when its speaker's NEXT utterance begins — otherwise a
// dropped Complete silently deletes a sentence from the record.
#include "caption-stream.h"
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

static CaptionEvent ev(const char *id, const char *who, const char *text,
                       CaptionOp op, uint64_t ns)
{
    return CaptionEvent{id, who, 7u, text, op, ns};
}

int main()
{
    CaptionStream s;

    // --- Add then Update rewrites the same line; nothing finalizes yet ---
    check(s.apply(ev("m1", "Ada", "hel", CaptionOp::Add, 100)).empty(),
          "Add finalized a line prematurely");
    check(s.apply(ev("m1", "Ada", "hello there", CaptionOp::Update, 200)).empty(),
          "Update finalized a line prematurely");
    check(s.display_line() == "Ada: hello there",
          "display_line did not show the updated partial");

    // --- Complete finalizes with the FIRST arrival ns (utterance start) ---
    auto f = s.apply(ev("m1", "Ada", "hello there.", CaptionOp::Complete, 300));
    check(f.size() == 1 && f[0].text == "hello there." && f[0].speaker == "Ada",
          "Complete did not finalize the coalesced text");
    check(!f.empty() && f[0].first_seen_ns == 100,
          "finalized line lost the utterance-START timestamp; alignment to the "
          "recording needs when speech began, not when ASR finished");

    // --- A never-Completed utterance finalizes when the same speaker Adds a new id ---
    s.apply(ev("m2", "Ada", "lost sentence", CaptionOp::Add, 400));
    auto g = s.apply(ev("m3", "Ada", "next", CaptionOp::Add, 500));
    check(g.size() == 1 && g[0].text == "lost sentence",
          "an utterance with no Complete vanished when the next one started");

    // --- Delete retracts: no final, and the speaker's latest line is cleared ---
    s.apply(ev("m3", "Ada", "oops", CaptionOp::Update, 600));
    auto h = s.apply(ev("m3", "Ada", "", CaptionOp::Delete, 700));
    check(h.empty(), "Delete produced a finalized line");
    check(s.latest().find("Ada") == s.latest().end() ||
              s.latest().at("Ada").empty(),
          "Delete left the retracted text on the speaker's latest line");

    // --- An Update for an unseen id behaves as Add (plugin attached mid-utterance) ---
    auto i = s.apply(ev("m9", "Grace", "mid join", CaptionOp::Update, 800));
    check(i.empty() && s.display_line() == "Grace: mid join",
          "an Update for an unknown message id was dropped instead of adopted");

    // --- A Complete for an unseen id still reaches the transcript ---
    auto j = s.apply(ev("mX", "Ada", "orphan final.", CaptionOp::Complete, 900));
    check(j.size() == 1 && j[0].text == "orphan final." && j[0].first_seen_ns == 900,
          "a Complete with no prior Add was dropped");

    // --- clear() empties everything (meeting boundary) ---
    s.clear();
    check(s.latest().empty() && s.display_line().empty(),
          "clear() left state from the previous meeting");

    if (failures == 0) std::cout << "caption-stream: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Add to `CMakeLists.txt` immediately after the `add_test(NAME CoreVideoTileRetry ...)` block:

```cmake
    # Caption coalescing (Zoom live transcription Add/Update/Delete/Complete).
    # Pure logic, no Qt/OBS/SDK — see src/caption-stream.h.
    add_executable(CoreVideoCaptionStreamTest
        tests/caption-stream-test.cpp
    )
    target_include_directories(CoreVideoCaptionStreamTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoCaptionStream
             COMMAND CoreVideoCaptionStreamTest)
```

Then:

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoCaptionStreamTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'caption-stream.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/caption-stream.h`:

```cpp
#pragma once
//
// caption-stream.h — coalesces Zoom live transcription messages into lines.
//
// Zoom delivers one utterance as Add, a burst of Updates rewriting the same
// message id, then (usually) Complete. "Usually" is the point of this file:
// a Complete can simply not arrive, and a coalescer that only finalizes on
// Complete silently deletes that sentence from the transcript. The fallback
// rule — finalize a speaker's pending utterance when their NEXT one begins —
// is what the test pins hardest.
//
// Values of CaptionOp mirror SDKLiveTranscriptionOperationType from
// meeting_closedcaption_interface.h numerically (None=0 .. NotSupported=5),
// so the engine can send the raw enum and the plugin static_casts it.
//
// Keyed by message id for coalescing, by speaker NAME for the latest-line
// view: user ids are meeting-scoped and never persisted (project rule).
// Free of Qt/OBS/SDK so a plain executable can pin it.
//
#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum class CaptionOp { None = 0, Add = 1, Update = 2, Delete = 3, Complete = 4, NotSupported = 5 };

struct CaptionEvent {
    std::string msg_id;
    std::string speaker;
    uint32_t speaker_id = 0;   // wire correlation only; never stored past apply()
    std::string text;
    CaptionOp op = CaptionOp::None;
    uint64_t arrival_ns = 0;   // plugin-side os_gettime_ns() at pipe arrival
};

struct CaptionFinal {
    std::string speaker;
    std::string text;
    uint64_t first_seen_ns = 0; // arrival of the utterance's FIRST message
};

class CaptionStream {
public:
    // Feeds one wire event; returns every line this event finalized (0..2:
    // a stale pending flushed by a new Add, plus this event's own Complete).
    std::vector<CaptionFinal> apply(const CaptionEvent &ev)
    {
        std::vector<CaptionFinal> out;
        if (ev.op == CaptionOp::Delete) {
            m_pending.erase(ev.msg_id);
            m_latest.erase(ev.speaker);
            return out;
        }
        if (ev.op == CaptionOp::Complete) {
            auto it = m_pending.find(ev.msg_id);
            const uint64_t first =
                it != m_pending.end() ? it->second.first_seen_ns : ev.arrival_ns;
            m_pending.erase(ev.msg_id);
            if (!ev.text.empty()) {
                out.push_back({ev.speaker, ev.text, first});
                m_latest[ev.speaker] = ev.text;
                m_display_speaker = ev.speaker;
            }
            return out;
        }
        if (ev.op != CaptionOp::Add && ev.op != CaptionOp::Update)
            return out; // None / NotSupported carry no text worth acting on

        // A new Add while the same speaker has a DIFFERENT utterance pending
        // means its Complete was lost: flush it now or lose it forever.
        if (ev.op == CaptionOp::Add) {
            for (auto it = m_pending.begin(); it != m_pending.end();) {
                if (it->second.speaker == ev.speaker && it->first != ev.msg_id) {
                    if (!it->second.text.empty())
                        out.push_back({it->second.speaker, it->second.text,
                                       it->second.first_seen_ns});
                    it = m_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto [it, inserted] = m_pending.try_emplace(
            ev.msg_id, Pending{ev.speaker, ev.text, ev.arrival_ns});
        if (!inserted)
            it->second.text = ev.text; // Update: rewrite, keep first_seen_ns
        m_latest[ev.speaker] = ev.text;
        m_display_speaker = ev.speaker;

        // Bound the pending table: a peer that streams Adds without ever
        // completing them must not grow memory without limit. 64 concurrent
        // half-finished utterances is far past any real meeting.
        while (m_pending.size() > 64) {
            auto oldest = m_pending.begin();
            for (auto p = m_pending.begin(); p != m_pending.end(); ++p)
                if (p->second.first_seen_ns < oldest->second.first_seen_ns)
                    oldest = p;
            if (!oldest->second.text.empty())
                out.push_back({oldest->second.speaker, oldest->second.text,
                               oldest->second.first_seen_ns});
            m_pending.erase(oldest);
        }
        return out;
    }

    const std::map<std::string, std::string> &latest() const { return m_latest; }

    // The line the OBS text source shows: the most recently active speaker.
    std::string display_line() const
    {
        auto it = m_latest.find(m_display_speaker);
        if (it == m_latest.end() || it->second.empty()) return {};
        return m_display_speaker + ": " + it->second;
    }

    void clear()
    {
        m_pending.clear();
        m_latest.clear();
        m_display_speaker.clear();
    }

private:
    struct Pending {
        std::string speaker;
        std::string text;
        uint64_t first_seen_ns = 0;
    };
    std::map<std::string, Pending> m_pending;      // by msg_id
    std::map<std::string, std::string> m_latest;   // by speaker name
    std::string m_display_speaker;
};
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --target CoreVideoCaptionStreamTest --parallel 8
cd build && ctest -C Release -R CoreVideoCaptionStream --output-on-failure
```

Expected: PASS, `caption-stream: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/caption-stream.h tests/caption-stream-test.cpp CMakeLists.txt
git commit -m "feat(captions): caption coalescing state machine with lost-Complete flush"
```

---

### Task 3: Transcript line formatter

The sidecar log's whole value is that its timestamps mean something against the ISO files. Every ISO session anchors `started_ns` to `os_gettime_ns()` (`src/zoom-iso-recorder.cpp:491`), so a transcript line's offset from a recording-start anchor on the same clock is directly comparable. The formatting is trivial; the clamp is not — a caption whose utterance began *before* the recording started must clamp to 00:00:00.000, because unsigned subtraction of a later anchor wraps to a ~584-year offset.

**Files:**
- Create: `src/caption-transcript-log.h`
- Test: `tests/caption-transcript-log-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoCaptionStream` block from Task 2)

**Interfaces:**
- Consumes: nothing.
- Produces: `std::string caption_transcript_line(uint64_t anchor_ns, uint64_t event_ns, const std::string &speaker, const std::string &text)` — Task 6 calls it once per `CaptionFinal`.

- [ ] **Step 1: Write the failing test**

Create `tests/caption-transcript-log-test.cpp`:

```cpp
// tests/caption-transcript-log-test.cpp
// Pins the sidecar transcript's line format and the pre-anchor clamp: an
// utterance that began before the recording did must read 00:00:00.000, not
// wrap uint64 into a ~584-year offset.
#include "caption-transcript-log.h"
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

int main()
{
    // 1h 2m 3s 456ms after the anchor.
    const uint64_t anchor = 1000000000ull;
    const uint64_t at = anchor + ((3600ull + 120ull + 3ull) * 1000ull + 456ull) * 1000000ull;
    check(caption_transcript_line(anchor, at, "Ada", "hello.") ==
              "[01:02:03.456] Ada: hello.\n",
          "line format changed — downstream alignment tooling parses this shape");

    // --- Pre-anchor clamps to zero instead of wrapping ---
    check(caption_transcript_line(anchor, anchor - 5, "Ada", "early") ==
              "[00:00:00.000] Ada: early\n",
          "a caption from before the recording start wrapped instead of clamping");

    // --- Embedded newlines flatten: one caption is always exactly one line ---
    check(caption_transcript_line(anchor, anchor, "Ada", "a\r\nb\nc") ==
              "[00:00:00.000] Ada: a b c\n",
          "an embedded newline split one caption across transcript lines");

    if (failures == 0) std::cout << "caption-transcript-log: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Register in `CMakeLists.txt` after the `CoreVideoCaptionStream` block:

```cmake
    # Sidecar transcript formatting. The clamp is the invariant: unsigned
    # pre-anchor subtraction wraps to a ~584-year offset.
    add_executable(CoreVideoCaptionTranscriptLogTest
        tests/caption-transcript-log-test.cpp
    )
    target_include_directories(CoreVideoCaptionTranscriptLogTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoCaptionTranscriptLog
             COMMAND CoreVideoCaptionTranscriptLogTest)
```

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoCaptionTranscriptLogTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'caption-transcript-log.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/caption-transcript-log.h`:

```cpp
#pragma once
//
// caption-transcript-log.h — one finalized caption as one transcript line.
//
// Offsets are (event_ns - anchor_ns) on the os_gettime_ns() clock — the SAME
// clock every ISO session's started_ns uses (src/zoom-iso-recorder.cpp:491),
// which is the entire alignment story. The SDK's own GetTimeStamp() is time_t
// seconds with an undocumented basis and is deliberately not used here.
// Free of Qt/OBS/SDK so a plain executable can pin it.
//
#include <cstdint>
#include <cstdio>
#include <string>

inline std::string caption_transcript_line(uint64_t anchor_ns,
                                           uint64_t event_ns,
                                           const std::string &speaker,
                                           const std::string &text)
{
    // Clamp, never wrap: captions can predate the recording start (the
    // utterance began, then the operator hit record).
    const uint64_t off_ms =
        event_ns > anchor_ns ? (event_ns - anchor_ns) / 1000000ull : 0ull;
    const unsigned hours = static_cast<unsigned>(off_ms / 3600000ull);
    const unsigned mins  = static_cast<unsigned>((off_ms / 60000ull) % 60ull);
    const unsigned secs  = static_cast<unsigned>((off_ms / 1000ull) % 60ull);
    const unsigned ms    = static_cast<unsigned>(off_ms % 1000ull);

    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ",
                  hours, mins, secs, ms);

    // One caption is one line, whatever the ASR put in the text. \r\n first
    // so it flattens to one space, not two.
    std::string flat = text;
    std::size_t pos;
    while ((pos = flat.find("\r\n")) != std::string::npos)
        flat.replace(pos, 2, " ");
    for (char &c : flat)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';

    return std::string(stamp) + speaker + ": " + flat + "\n";
}
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --target CoreVideoCaptionTranscriptLogTest --parallel 8
cd build && ctest -C Release -R CoreVideoCaptionTranscriptLog --output-on-failure
```

Expected: PASS, `caption-transcript-log: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/caption-transcript-log.h tests/caption-transcript-log-test.cpp CMakeLists.txt
git commit -m "feat(captions): transcript line formatter anchored to the ISO clock"
```

---

### Task 4: Engine — `EngineCaptions` controller, event sink, and the caption wire

The engine side. `IClosedCaptionController` is acquired the way every other controller is: in `EngineMeetingEvent::onMeetingStatusChanged`'s `MEETING_STATUS_INMEETING` case (`engine/src/main.cpp:1007-1031`, beside `m_participants->attach` and `m_share_engine->attach`), detached on `DISCONNECTING`/`ENDED`. Start/stop run on the command-loop thread; message callbacks arrive on SDK threads and only build a line and call `EngineIpc::write` — which is exactly what `engine/src/engine-writer.h` exists to make safe. Messages are forwarded whenever the sink is attached, not only when *we* started transcription: the host starting captions from the Zoom client must light up our stream too.

No unit test — SDK-bound; verification is Task 7's live pass, the same ruling as talkback Milestone 1 Tasks 3–4. The compile is the check at this stage.

**Files:**
- Create: `engine/src/engine-captions.h`, `engine/src/engine-captions.cpp`
- Modify: `engine/src/main.cpp` (EngineMeetingEvent member + attach/detach at `:1007-1031`; `static EngineCaptions captions;` beside `static EngineTalkback talkback;` at `:1290`; command branches beside `IpcCommand::TalkbackNominate` at `:1664`)
- Modify: `CMakeLists.txt:391-396` (add `engine/src/engine-captions.cpp` to `ENGINE_SOURCES`)

**Interfaces:**
- Consumes: `IpcCommand::CaptionsStart` / `CaptionsStop` (Task 1); `EngineIpc::write` (`engine/src/engine-writer.h`); `json_escape`/`zchar_to_utf8` (`engine/src/engine-json.h`).
- Produces: `class EngineCaptions` with `void attach(ZOOMSDK::IClosedCaptionController *ctrl)`, `void detach()`, `void start()`, `void stop()`. Wire lines: `{"cmd":"captions","stage":...}` (control/status reports) and `{"cmd":"caption",...}` (one per transcription message). Task 5 consumes both shapes.

- [ ] **Step 1: Write the header (the shape is the commitment)**

Create `engine/src/engine-captions.h`:

```cpp
#pragma once
//
// engine-captions.h — Zoom live transcription -> E2P caption stream.
//
// Every media path in this codebase moves pixels or PCM; this one moves TEXT,
// which is why it needs none of the SHM machinery: a caption is a few dozen
// bytes a few times a second, well inside what the line-JSON pipe carries as
// control traffic. Messages are forwarded whenever the sink is attached —
// transcription started by the HOST from the Zoom client must reach OBS the
// same as transcription we started ourselves.
//
// Callback threading: onLiveTranscriptionMsgInfoReceived arrives on an SDK
// thread. It builds one string and calls EngineIpc::write (serialised); it
// takes no other lock and touches no controller method — the same discipline
// engine-writer.h's header comment demands of every SDK callback.
//
#include "zoom_sdk.h"
#include "meeting_service_interface.h"
#include "meeting_closedcaption_interface.h"

#include <atomic>

class EngineCaptions : public ZOOMSDK::IClosedCaptionControllerEvent {
public:
    // Meeting lifecycle (command-loop / meeting-event thread).
    void attach(ZOOMSDK::IClosedCaptionController *ctrl);
    void detach();

    // Command branches (command-loop thread only, like every SDK call we
    // initiate). Each reports its gate ladder as "captions" stage lines.
    void start();
    void stop();

    // IClosedCaptionControllerEvent — all 13, the SDK interface is abstract.
    void onAssignedToSendCC(bool bAssigned) override;
    void onClosedCaptionMsgReceived(const zchar_t *ccMsg, unsigned int sender_id,
                                    time_t time) override;
    void onLiveTranscriptionStatus(ZOOMSDK::SDKLiveTranscriptionStatus status) override;
    void onOriginalLanguageMsgReceived(
        ZOOMSDK::ILiveTranscriptionMessageInfo *messageInfo) override;
    void onLiveTranscriptionMsgInfoReceived(
        ZOOMSDK::ILiveTranscriptionMessageInfo *messageInfo) override;
    void onLiveTranscriptionMsgError(
        ZOOMSDK::ILiveTranscriptionLanguage *spokenLanguage,
        ZOOMSDK::ILiveTranscriptionLanguage *transcriptLanguage) override;
    void onRequestForLiveTranscriptReceived(unsigned int requester_id,
                                            bool bAnonymous) override;
    void onRequestLiveTranscriptionStatusChange(bool bEnabled) override;
    void onCaptionStatusChanged(bool bEnabled) override;
    void onStartCaptionsRequestReceived(ZOOMSDK::ICCRequestHandler *handler) override;
    void onStartCaptionsRequestApproved() override;
    void onManualCaptionStatusChanged(bool bEnabled) override;
    void onSpokenLanguageChanged(
        ZOOMSDK::ILiveTranscriptionLanguage *spokenLanguage) override;

private:
    void report(const std::string &stage, const std::string &fields);
    void forward_message(ZOOMSDK::ILiveTranscriptionMessageInfo *info);

    // Written on attach/detach, read by SDK-thread callbacks: atomic so a
    // late callback racing detach() forwards or drops cleanly, never tears.
    std::atomic<ZOOMSDK::IClosedCaptionController *> m_ctrl{nullptr};
};
```

- [ ] **Step 2: Run the build to verify it fails**

Add `engine/src/engine-captions.cpp` to `ENGINE_SOURCES` (`CMakeLists.txt:391`), then:

```sh
cmake -S . -B build && cmake --build build --config Release --target ZoomObsEngine --parallel 8
```

Expected: FAIL — `engine-captions.cpp` does not exist.

- [ ] **Step 3: Write the implementation**

Create `engine/src/engine-captions.cpp`:

```cpp
#include "engine-captions.h"
#include "engine-writer.h" // EngineIpc::write — include, never forward-declare
#include "engine-json.h"   // json_escape / zchar_to_utf8

#include <string>

void EngineCaptions::report(const std::string &stage, const std::string &fields)
{
    std::string line = R"({"cmd":"captions","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

void EngineCaptions::attach(ZOOMSDK::IClosedCaptionController *ctrl)
{
    m_ctrl.store(ctrl, std::memory_order_release);
    if (!ctrl) {
        report("attach", R"("ok":false,"reason":"null_controller")");
        return;
    }
    const ZOOMSDK::SDKError err = ctrl->SetEvent(this);
    report("attach", std::string(R"("ok":)") +
           (err == ZOOMSDK::SDKERR_SUCCESS ? "true" : "false") +
           R"(,"code":)" + std::to_string(static_cast<int>(err)));
}

void EngineCaptions::detach()
{
    ZOOMSDK::IClosedCaptionController *ctrl =
        m_ctrl.exchange(nullptr, std::memory_order_acq_rel);
    if (ctrl) ctrl->SetEvent(nullptr);
}

void EngineCaptions::start()
{
    ZOOMSDK::IClosedCaptionController *ctrl = m_ctrl.load(std::memory_order_acquire);
    if (!ctrl) {
        report("start", R"("ok":false,"reason":"no_controller")");
        return;
    }
    // The gate ladder, every rung reported — a captions_start that produces
    // silence must name which rung it fell off, not leave the operator
    // guessing between entitlement, permission, and a hang.
    report("meeting_supported", std::string(R"("supported":)") +
           (ctrl->IsMeetingSupportCC() ? "true" : "false"));
    report("feature_enabled", std::string(R"("enabled":)") +
           (ctrl->IsLiveTranscriptionFeatureEnabled() ? "true" : "false"));
    report("lt_status", R"("status":)" +
           std::to_string(static_cast<int>(ctrl->GetLiveTranscriptionStatus())));
    const bool can = ctrl->CanStartLiveTranscription();
    report("can_start", std::string(R"("can":)") + (can ? "true" : "false"));
    // Attempt even when can=false: like the talkback probe's per-user gate,
    // the contrast between the query and the real outcome is itself data.
    const ZOOMSDK::SDKError err = ctrl->StartLiveTranscription();
    report("start", std::string(R"("ok":)") +
           (err == ZOOMSDK::SDKERR_SUCCESS ? "true" : "false") +
           R"(,"code":)" + std::to_string(static_cast<int>(err)));
}

void EngineCaptions::stop()
{
    ZOOMSDK::IClosedCaptionController *ctrl = m_ctrl.load(std::memory_order_acquire);
    if (!ctrl) {
        report("stop", R"("ok":false,"reason":"no_controller")");
        return;
    }
    const ZOOMSDK::SDKError err = ctrl->StopLiveTranscription();
    report("stop", std::string(R"("ok":)") +
           (err == ZOOMSDK::SDKERR_SUCCESS ? "true" : "false") +
           R"(,"code":)" + std::to_string(static_cast<int>(err)));
}

void EngineCaptions::forward_message(ZOOMSDK::ILiveTranscriptionMessageInfo *info)
{
    if (!info) return;
    if (!m_ctrl.load(std::memory_order_acquire)) return; // detached: drop
    // GetTimeStamp() is time_t (whole seconds, basis undocumented) — carried
    // for the record; the plugin aligns on its own arrival os_gettime_ns().
    EngineIpc::write(
        R"({"cmd":"caption","msg_id":")" + json_escape(zchar_to_utf8(info->GetMessageID())) +
        R"(","speaker_id":)" + std::to_string(info->GetSpeakerID()) +
        R"(,"speaker":")" + json_escape(zchar_to_utf8(info->GetSpeakerName())) +
        R"(","text":")" + json_escape(zchar_to_utf8(info->GetMessageContent())) +
        R"(","op":)" + std::to_string(static_cast<int>(info->GetMessageOperationType())) +
        R"(,"ts":)" + std::to_string(static_cast<long long>(info->GetTimeStamp())) + "}");
}

void EngineCaptions::onLiveTranscriptionMsgInfoReceived(
    ZOOMSDK::ILiveTranscriptionMessageInfo *messageInfo)
{
    forward_message(messageInfo);
}

// Deliberately NOT forwarded: with translation enabled the SDK delivers the
// same utterance on BOTH this and onLiveTranscriptionMsgInfoReceived, and a
// duplicated stream would double every transcript line. One channel, chosen
// once.
void EngineCaptions::onOriginalLanguageMsgReceived(
    ZOOMSDK::ILiveTranscriptionMessageInfo *) {}

void EngineCaptions::onLiveTranscriptionStatus(ZOOMSDK::SDKLiveTranscriptionStatus status)
{
    report("lt_status", R"("status":)" + std::to_string(static_cast<int>(status)));
}

void EngineCaptions::onCaptionStatusChanged(bool bEnabled)
{
    report("caption_status", std::string(R"("enabled":)") + (bEnabled ? "true" : "false"));
}

// Observed but out of scope for this feature — logged so a live run that hits
// them is diagnosable, no action taken.
void EngineCaptions::onAssignedToSendCC(bool) {}
void EngineCaptions::onClosedCaptionMsgReceived(const zchar_t *, unsigned int, time_t) {}
void EngineCaptions::onLiveTranscriptionMsgError(ZOOMSDK::ILiveTranscriptionLanguage *,
                                                 ZOOMSDK::ILiveTranscriptionLanguage *) {}
void EngineCaptions::onRequestForLiveTranscriptReceived(unsigned int, bool) {}
void EngineCaptions::onRequestLiveTranscriptionStatusChange(bool) {}
void EngineCaptions::onStartCaptionsRequestReceived(ZOOMSDK::ICCRequestHandler *) {}
void EngineCaptions::onStartCaptionsRequestApproved() {}
void EngineCaptions::onManualCaptionStatusChanged(bool) {}
void EngineCaptions::onSpokenLanguageChanged(ZOOMSDK::ILiveTranscriptionLanguage *) {}
```

- [ ] **Step 3a: Wire `main.cpp`**

Add `#include "engine-captions.h"` with the other engine includes. Beside `static EngineTalkback talkback;` (`engine/src/main.cpp:1290`) add `static EngineCaptions captions;` — static for the same lifetime reason documented there. Give `EngineMeetingEvent` a member `EngineCaptions *m_captions = nullptr;` and a setter `void attach_captions(EngineCaptions *c) { m_captions = c; }` (mirroring `attach_talkback`), call `meeting_event.attach_captions(&captions);` next to `participants.attach_talkback(...)` (`:1297`). In `onMeetingStatusChanged`'s `MEETING_STATUS_INMEETING` case (`:1007-1031`), after the share-controller attach:

```cpp
            if (m_captions && m_meeting_svc && *m_meeting_svc)
                m_captions->attach(
                    (*m_meeting_svc)->GetMeetingClosedCaptionController());
```

and in the `DISCONNECTING`/`ENDED` case beside `m_participants->detach()` (`:1097`): `if (m_captions) m_captions->detach();`. In the command loop, beside the `IpcCommand::TalkbackNominate` branch (`:1664`):

```cpp
        } else if (command == IpcCommand::CaptionsStart) {
            captions.start();
        } else if (command == IpcCommand::CaptionsStop) {
            captions.stop();
```

- [ ] **Step 4: Build and run the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: engine links; full suite green (unchanged count — this task adds no tests but must break none).

- [ ] **Step 5: Commit**

```sh
git add engine/src/engine-captions.h engine/src/engine-captions.cpp engine/src/main.cpp CMakeLists.txt
git commit -m "feat(captions): engine live-transcription sink and caption wire"
```

---

### Task 5: Plugin — caption events, control API, and the OBS text source

The plugin side of the wire. `{"cmd":"caption"}` lines land in `ZoomEngineClient::handle_event` (`src/zoom-engine-client.cpp:1299`) on the pipe reader thread — the ordered control path, which is what a coalescing state machine needs (an `Update` processed before its `Add` would resurrect deleted text). `{"cmd":"captions"}` stage lines are logged verbatim and stashed for status, exactly the `talkback_probe` pattern (`:1351-1366`). The control API gains `captions_start`/`captions_stop` (fire-and-acknowledge, gates reported as stage lines), `captions_configure` (names the text source), and `captions_status` (poll). The text source update runs on the reader thread via `obs_get_source_by_name`/`obs_source_update` — the calls `src/talkback-tap.cpp:20` and `src/zoom-tiles-audio.cpp` already make off the UI thread; never `obs_enum_sources` here (it takes libobs's source mutex per the `zoom-dock.h:155` m2 note), and never while holding `m_mtx`.

No new unit test — the coalescing logic this task wires is already pinned by Task 2, and the rest is Qt/OBS/socket wiring verified live in Task 7 (same ruling as talkback M1 Task 5). If a future round finds a mapping bug here, extract the mapping into a dispatch header first, like `src/talkback-nomination-dispatch.h` — do not test through the socket.

**Files:**
- Modify: `src/zoom-engine-client.h` (declare methods + members), `src/zoom-engine-client.cpp` (send methods beside `talkback_probe` at `:907`; `handle_event` branches beside `talkback_probe`'s at `:1351`)
- Modify: `src/zoom-control-server.cpp` (four branches beside `talkback_probe`'s at `:851`)

**Interfaces:**
- Consumes: `CaptionStream`/`CaptionEvent`/`CaptionOp` (Task 2); wire shapes from Task 4; `ZoomIsoRecorder::record_caption` (Task 6 — until Task 6 lands, guard the call with `#if 0` is **not** allowed; instead Task 6 must merge before this task's sidecar line compiles, so implement Tasks 6's recorder method first if executing out of order, or land this task's step 3 exactly as written which only calls existing API and leaves one clearly-marked call to add in Task 6).
- Produces: `ZoomEngineClient::captions_start()`, `captions_stop()`, `set_caption_text_source(const std::string&)`, `std::string caption_text_source() const`, `QJsonObject captions_status_json()`; control commands `captions_start`, `captions_stop`, `captions_configure`, `captions_status`.

- [ ] **Step 1: Client members and send methods**

In `src/zoom-engine-client.h`, beside the talkback members, add:

```cpp
    void captions_start();
    void captions_stop();
    void set_caption_text_source(const std::string &name);
    std::string caption_text_source() const;
    QJsonObject captions_status_json();
```

and private state (guarded by the existing `m_mtx`):

```cpp
    CaptionStream m_caption_stream;
    std::string m_caption_text_source;   // OBS text source name; "" = disabled
    std::string m_caption_last_display;  // last text pushed, to skip no-op updates
    std::string m_captions_status_line;  // latest {"cmd":"captions"} line, verbatim
```

In `src/zoom-engine-client.cpp`, beside `talkback_probe` (`:907`):

```cpp
void ZoomEngineClient::captions_start()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"captions_start"})");
}

void ZoomEngineClient::captions_stop()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"captions_stop"})");
}
```

- [ ] **Step 2: `handle_event` branches**

In `handle_event`, beside the `talkback_probe` branch (`:1351`), add:

```cpp
    if (cmd == "captions") {
        // Gate-ladder stage lines: verbatim to the log, latest stashed for
        // captions_status — the talkback_probe pattern, same reasons.
        blog(LOG_INFO, "[obs-zoom-plugin] captions: %s", line.c_str());
        std::lock_guard<std::mutex> lk(m_mtx);
        m_captions_status_line = line;
        return;
    }
    if (cmd == "caption") {
        CaptionEvent ev;
        ev.msg_id     = obj.value("msg_id").toString().toStdString();
        ev.speaker    = obj.value("speaker").toString().toStdString();
        ev.speaker_id = static_cast<uint32_t>(obj.value("speaker_id").toInt(0));
        ev.text       = obj.value("text").toString().toStdString();
        ev.op         = static_cast<CaptionOp>(obj.value("op").toInt(0));
        ev.arrival_ns = os_gettime_ns(); // the ISO clock — alignment happens HERE
        std::vector<CaptionFinal> finals;
        std::string display;
        bool display_changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            finals = m_caption_stream.apply(ev);
            display = m_caption_stream.display_line();
            display_changed = display != m_caption_last_display;
            if (display_changed) m_caption_last_display = display;
        }
        // Sidecar first (finalized lines must not depend on OBS source state).
        for (const CaptionFinal &f : finals)
            ZoomIsoRecorder::instance().record_caption(f.speaker, f.text,
                                                       f.first_seen_ns);
        // Text source update OUTSIDE m_mtx: obs_source_update can fan out to
        // source callbacks, and holding our lock across libobs is how audio
        // paths have deadlocked before. Skip no-op updates — every redundant
        // update invalidates the text source's texture for nothing.
        const std::string target = caption_text_source();
        if (display_changed && !target.empty()) {
            obs_source_t *src = obs_get_source_by_name(target.c_str());
            if (src) {
                obs_data_t *settings = obs_data_create();
                obs_data_set_string(settings, "text", display.c_str());
                obs_source_update(src, settings);
                obs_data_release(settings);
                obs_source_release(src);
            }
        }
        return;
    }
```

with the accessors:

```cpp
void ZoomEngineClient::set_caption_text_source(const std::string &name)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_caption_text_source = name;
}

std::string ZoomEngineClient::caption_text_source() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_caption_text_source;
}

QJsonObject ZoomEngineClient::captions_status_json()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    QJsonObject speakers;
    for (const auto &entry : m_caption_stream.latest())
        speakers[QString::fromStdString(entry.first)] =
            QString::fromStdString(entry.second);
    return QJsonObject{
        {"text_source", QString::fromStdString(m_caption_text_source)},
        {"display", QString::fromStdString(m_caption_stream.display_line())},
        {"speakers", speakers},
        {"last_stage", QString::fromStdString(m_captions_status_line)},
    };
}
```

Also add `m_caption_stream.clear(); m_caption_last_display.clear();` inside `handle_event`'s existing `"left"` branch (`:1453`) and in `stop_for_reconnect()` — the two world-reset points the talkback nomination record already uses, and for the same reason: a new meeting's captions must not coalesce against the old meeting's pending utterances.

- [ ] **Step 3: Control API branches**

In `src/zoom-control-server.cpp`, beside `talkback_probe` (`:851`):

```cpp
    if (cmd == "captions_start" || cmd == "captions_stop") {
        if (!ZoomEngineClient::instance().is_running()) {
            write_response(socket, {{"ok", false}, {"error", "engine_not_running"},
                {"message", "The Zoom engine is not running."}});
            return;
        }
        if (ZoomEngineClient::instance().state() != MeetingState::InMeeting) {
            write_response(socket, {{"ok", false}, {"error", "not_in_meeting"},
                {"message", "Join the meeting before toggling live transcription."}});
            return;
        }
        if (cmd == "captions_start")
            ZoomEngineClient::instance().captions_start();
        else
            ZoomEngineClient::instance().captions_stop();
        write_response(socket, {{"ok", true},
            {"note", "sent; watch the OBS log for captions stages"}});
        return;
    }

    if (cmd == "captions_configure") {
        // "" is legal and disables the text-source update — do not refuse it;
        // it is the off switch.
        const QString source = req.value("text_source").toString();
        ZoomEngineClient::instance().set_caption_text_source(
            source.toStdString());
        write_response(socket, {{"ok", true},
            {"text_source", source}});
        return;
    }

    if (cmd == "captions_status") {
        write_response(socket, {{"ok", true},
            {"captions", ZoomEngineClient::instance().captions_status_json()}});
        return;
    }
```

- [ ] **Step 4: Build and run the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: all green. (If Task 6 has not merged yet, the `record_caption` call will not compile — implement Task 6's Step 3 recorder method first; the task ordering here assumes in-order execution.)

- [ ] **Step 5: Commit**

```sh
git add src/zoom-engine-client.h src/zoom-engine-client.cpp src/zoom-control-server.cpp
git commit -m "feat(captions): caption stream, control API surface, and text-source drive"
```

---

### Task 6: Sidecar transcript in the ISO recorder

One transcript file per ISO run, not per source: captions are a meeting-wide stream, and per-source duplication would write the same sentence eight times. The anchor is `os_gettime_ns()` taken inside `start()`'s locked block (`src/zoom-iso-recorder.cpp:247-262`) — the same clock and the same moment the run's sessions begin anchoring against, so `[00:03:07.210]` in the transcript is directly seekable in any ISO file whose own `started_ns` is known (each session's start is already logged). Writes happen under `m_mtx` with an `fflush` per line: a transcript is tens of bytes per second, and a crash mid-show must not lose the show's transcript to a fat stdio buffer — the mirror image of why the *video* path is never allowed to block there.

**Files:**
- Modify: `src/zoom-iso-recorder.h` (declare `record_caption` + members)
- Modify: `src/zoom-iso-recorder.cpp` (open in `start()`, write, close in `stop()`)
- Test: covered by `tests/caption-transcript-log-test.cpp` (Task 3) for everything pure; the file plumbing below follows the `WavFile` pattern already in this class and is exercised live in Task 7.

**Interfaces:**
- Consumes: `caption_transcript_line` (Task 3).
- Produces: `void ZoomIsoRecorder::record_caption(const std::string &speaker, const std::string &text, uint64_t first_seen_ns);` — called from Task 5's `handle_event` caption branch.

- [ ] **Step 1: Declare**

In `src/zoom-iso-recorder.h`, beside `record_audio_frame` (`:49`):

```cpp
    // Appends one finalized caption line to this run's sidecar transcript.
    // No-op when not recording. first_seen_ns is on the os_gettime_ns()
    // clock (the plugin's arrival stamp for the utterance's first message).
    void record_caption(const std::string &speaker,
                        const std::string &text,
                        uint64_t first_seen_ns);
```

and private members beside `m_completed_sessions` (`:155`):

```cpp
    FILE *m_transcript = nullptr;
    uint64_t m_transcript_anchor_ns = 0;
    QString m_transcript_path;
    bool m_transcript_write_failed = false;
```

- [ ] **Step 2: Open on `start()`, close on `stop()`**

In `start()`'s locked block (`src/zoom-iso-recorder.cpp:247-262`), before `m_active.store(true, ...)`:

```cpp
        // Sidecar transcript: one per run, anchored NOW on the same
        // os_gettime_ns() clock every session's started_ns uses. A failed
        // open degrades to no transcript, never to no recording — captions
        // are additive to the ISO run, not a precondition of it.
        m_transcript_anchor_ns = os_gettime_ns();
        m_transcript_write_failed = false;
        m_transcript_path = dir.absoluteFilePath(
            "transcript-" + QDateTime::currentDateTimeUtc()
                                .toString("yyyyMMdd-HHmmss") + ".txt");
        m_transcript = fopen(m_transcript_path.toUtf8().constData(), "wb");
        if (m_transcript) {
            const std::string header =
                "# CoreVideo transcript. Offsets from recording start "
                "(os_gettime_ns anchor " + std::to_string(m_transcript_anchor_ns) +
                "). Caption text lags speech by Zoom's ASR delay (~1-3s).\n";
            fwrite(header.data(), 1, header.size(), m_transcript);
        } else {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] ISO transcript could not be opened: %s",
                 m_transcript_path.toUtf8().constData());
        }
```

In `stop()`, inside the locked block that harvests sessions (`:289-300`):

```cpp
        if (m_transcript) {
            fclose(m_transcript);
            m_transcript = nullptr;
        }
```

- [ ] **Step 3: Write lines**

Add to `src/zoom-iso-recorder.cpp` (include `"caption-transcript-log.h"` at the top):

```cpp
void ZoomIsoRecorder::record_caption(const std::string &speaker,
                                     const std::string &text,
                                     uint64_t first_seen_ns)
{
    if (!m_active.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!m_transcript || m_transcript_write_failed) return;
    const std::string line =
        caption_transcript_line(m_transcript_anchor_ns, first_seen_ns,
                                speaker, text);
    if (fwrite(line.data(), 1, line.size(), m_transcript) != line.size()) {
        // Report once and stop writing — a full disk mid-show must not turn
        // every caption into a warning-log storm (the message-storm shape
        // this codebase has a live incident about).
        m_transcript_write_failed = true;
        blog(LOG_WARNING,
             "[obs-zoom-plugin] ISO transcript write failed; transcript "
             "abandoned at %s", m_transcript_path.toUtf8().constData());
        return;
    }
    fflush(m_transcript); // tens of bytes/sec; a crash must not eat the show
}
```

Also surface it: in `status_overview()` (`:382`), add `obj["transcript_path"] = m_transcript_path;` and `obj["transcript_write_failed"] = m_transcript_write_failed;`.

- [ ] **Step 4: Build and run the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: all green (Tasks 2–3 tests still pin the pure halves; this task adds none).

- [ ] **Step 5: Commit**

```sh
git add src/zoom-iso-recorder.h src/zoom-iso-recorder.cpp
git commit -m "feat(captions): sidecar transcript anchored to the ISO recording clock"
```

---

### Task 7: Live verification — the gate

The compile proves the API exists; only a meeting proves the account can use it. `StartLiveTranscription()`'s notes say only the host can start it unless multi-language transcription is allowed, and the account-level "Automated captions" setting gates the whole feature — which rung refuses, and with what code, is exactly what this run establishes. Install the matched pair first (both binaries, always — a DLL-only copy is this project's canonical mistake).

**Files:**
- Create: `docs/superpowers/notes/2026-08-29-captions-live-results.md` (record the actual output)

**Interfaces:**
- Consumes: everything above.
- Produces: the go/no-go for shipping captions in the next release.

- [ ] **Step 1: Setup** — a TEST meeting (never a live show), hosted by the entitled account with Automated Captions enabled in Zoom web settings; CoreVideo joined as host or co-host; two participants who will speak. Create an OBS text source named `Captions Lower Third`. Configure and start:

```sh
printf '{"cmd":"captions_configure","text_source":"Captions Lower Third"}\n' | nc 127.0.0.1 19870
printf '{"cmd":"captions_start"}\n' | nc 127.0.0.1 19870
```

- [ ] **Step 2: Read every rung.** Expected in the OBS log on the happy path, in order: `captions` stages `attach ok=true`, `meeting_supported supported=true`, `feature_enabled enabled=true`, `lt_status status=0`, `can_start can=true`, `start ok=true code=0`, then `lt_status status=1` (`SDK_LiveTranscription_Status_Start`) from the async callback, possibly preceded by `status=10` (`Connecting`). Any `ok=false` names its rung; `can=false` followed by `start ok=true` or the reverse is a meaningful contrast — record it, don't discard it.

- [ ] **Step 3: Confirm the stream.** Have both participants speak in turn. Verify: the text source shows `Name: text` updating as they speak, switching speakers; `printf '{"cmd":"captions_status"}\n' | nc 127.0.0.1 19870` returns both speakers under `"speakers"`; speaker NAMES match display names exactly.

- [ ] **Step 4: Confirm the sidecar.** Start an ISO recording via `iso_recording_start`, have someone speak a distinctive sentence roughly 10 s in, stop after ~30 s. Open `transcript-*.txt`: the sentence's `[00:00:0X.xxx]` offset must land within ~1–3 s *after* the word is audible at the same offset in that speaker's ISO MP4 (ASR delay is expected and documented in the file header; an offset *before* the audio, or minutes off, is a clock bug). Confirm `captions_stop` yields `stop ok=true` and the text source stops updating.

- [ ] **Step 5: Repeat Step 2 without co-host** — demote CoreVideo to plain participant, run `captions_start` again, record which rung refuses and with what code. This decides whether the UI must gate on role.

- [ ] **Step 6: Record and commit**

```sh
git add docs/superpowers/notes/2026-08-29-captions-live-results.md
git commit -m "docs: captions live verification results"
```

---

## Self-Review

**Placeholder scan:** none. Every code step contains the actual code. Tasks 4, 5, and 6 declare "no unit test" explicitly for SDK/OBS-bound wiring with a named verification route (Task 7), not as deferred work — the same ruling talkback M1 used, with the dispatch-header escape hatch named for the day it stops being true.

**Type consistency:** `CaptionOp` values mirror `SDKLiveTranscriptionOperationType` numerically (asserted in Task 2's header comment; Task 4 sends the raw int, Task 5 `static_cast`s it). `CaptionEvent`/`CaptionFinal`/`CaptionStream::apply` are defined in Task 2 and consumed in Task 5 with matching fields. `caption_transcript_line(uint64_t, uint64_t, const std::string&, const std::string&)` is defined in Task 3 and called in Task 6 with that exact argument order. `record_caption(const std::string&, const std::string&, uint64_t)` is declared in Task 6 and called in Task 5 with matching types — the one cross-task ordering hazard (5 calls 6's method) is stated in both tasks.

**SDK-name audit:** every SDK identifier used — `IClosedCaptionController`, `IClosedCaptionControllerEvent`, `ILiveTranscriptionMessageInfo` (`GetMessageID`/`GetSpeakerID`/`GetSpeakerName`/`GetMessageContent`/`GetTimeStamp`/`GetMessageOperationType`), `SDKLiveTranscriptionStatus`, `SDKLiveTranscriptionOperationType`, `IsMeetingSupportCC`, `IsLiveTranscriptionFeatureEnabled`, `GetLiveTranscriptionStatus`, `CanStartLiveTranscription`, `StartLiveTranscription`, `StopLiveTranscription`, `SetEvent`, `GetMeetingClosedCaptionController` — appears verbatim in `third_party/zoom-sdk/h/meeting_closedcaption_interface.h` or `meeting_service_interface.h:1234`. All 13 pure-virtual sink methods are overridden in Task 4. Nothing this plan needs is missing from the vendored headers; the only shortfall (seconds-resolution `GetTimeStamp`) is planned around, not worked around silently.
