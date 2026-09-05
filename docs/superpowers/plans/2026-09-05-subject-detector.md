# Subject Detector Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a stable, tested, CPU-only engine that returns a smoothed `SubjectFrame` (face box + 5-landmark-derived eyeline) per participant, at a fixed cost that does not grow with the roster.

**Architecture:** libfacedetection (YuNet weights compiled in as C arrays) is vendored as source and wrapped behind a one-method interface (`frame → box + landmarks`). The engine-IPC reader thread — which serves every source in the plugin — never runs detection: it only answers a single outstanding "send me one frame" request by copying I420 pixels into a one-slot inbox. A single dedicated worker thread drains that inbox, downscales-and-converts I420→BGR at ~320 px long edge, runs one detection, and feeds the result through a per-participant temporal smoother (deadband, rate limit, dropout hold). A round-robin schedule with an active-speaker boost issues exactly one detection per ~100 ms tick regardless of roster size.

**Tech Stack:** C++17, MSVC, CMake, libfacedetection (vendored, BSD-3), no OpenCV, no OpenMP, no test framework.

**Spec:** `docs/superpowers/specs/2026-09-05-panelist-feedback-design.md` (Subsystem 2 only)

## Global Constraints

- **Scope ends at the API.** This plan delivers `SubjectDetectorEngine::subject(participant_id) -> SubjectFrame`. It does NOT implement the loudness engine (Subsystem 1), Tiles auto-framing (3a), or the return overlay (3b). Those are separate plans by other authors and consume this API.
- **`SubjectFrame` is a published contract.** Two later plans consume it. Field names and types are fixed exactly as the spec writes them. Do not rename, reorder, or add required fields.
- **No test framework.** Plain `int main()` with a local `check()`/`near()`. Never gtest, never Catch.
- **Test naming:** target `CoreVideo<Thing>Test`, ctest name `CoreVideo<Thing>`, hand-registered inside `if(BUILD_TESTING)` in the root `CMakeLists.txt`. There is no `tests/CMakeLists.txt`.
- **No headless GPU harness exists and one has been ruled against.** Everything in this plan is pure CPU logic and must be testable off-rig.
- **New pure logic goes in a header-only file under `src/`** with a "why this exists" comment at the top, matching `src/audio-silence-fade.h` and `src/media-event-queue.h`.
- **Video format is I420 planar, BT.709, FULL range** (the engine already normalises limited→full every frame — `src/i420-range-expand.h`). Not NV12, not BGRA. Planes are tightly packed: Y of `w*h`, then U of `w*h/4`, then V of `w*h/4`.
- **libfacedetection requires BGR 3-channel.** A greyscale Y-plane shortcut is NOT valid for this CNN. The colour convert happens at the DOWNSCALED size (~320 px long edge), never at full resolution.
- **Detection must never run on the engine-IPC reader thread.** That thread dispatches frames for every source in the plugin; anything slow there head-of-line-blocks the whole plugin (2026-08-17 incident, see `src/media-event-queue.h`).
- **Do NOT enable `/openmp`** on the vendored library. We schedule one detection at a time on our own worker thread, which is what upstream recommends.
- **`/arch:AVX2` must provably reach the vendored library's compile line.** MSVC silently accepts AVX2 intrinsics under the default `/arch:SSE2` and emits a slow path; the only symptom is a detector that looks like a bad library.
- **Active speaker:** `ZoomEngineClient::active_speaker_id()` — the *directed* id, post hold/dwell. Not `raw_active_speaker_id()`. `SpeakerDirector` is **poll-only**: there is no observer list, so poll it; do not wait for events.
- **`ZoomEngineClient::roster()` deep-copies strings under a hot mutex.** Never call it per frame.
- **Dead code — do not build against:** `src/zoom-video-delegate.cpp`, `src/zoom-audio-delegate.*`, `src/zoom-audio-router.*`. None are in any CMake target.
- Build/verify: `cmake --build build --config Release --parallel 8` then `ctest -C Release --output-on-failure`, N/N green.

## File Structure

**Vendored (new — this repo has never vendored a library before; `third_party/` has only held the gitignored Zoom SDK):**

| File | Responsibility |
|---|---|
| `third_party/libfacedetection/LICENSE` | Upstream BSD-3 text, copied verbatim |
| `third_party/libfacedetection/README-COREVIDEO.md` | Provenance: upstream URL, pinned commit, exactly which files we copied and which we wrote |
| `third_party/libfacedetection/src/facedetectcnn.h` | Upstream, unmodified |
| `third_party/libfacedetection/src/facedetectcnn.cpp` | Upstream, unmodified |
| `third_party/libfacedetection/src/facedetectcnn-model.cpp` | Upstream, unmodified |
| `third_party/libfacedetection/src/facedetectcnn-data.cpp` | Upstream, unmodified (~446 KB of weights as static C arrays) |
| `third_party/libfacedetection/src/facedetection_export.h` | **Ours** — one line, `#define FACEDETECTION_EXPORT` |
| `third_party/libfacedetection/corevideo-avx2-assert.cpp` | **Ours** — compile-time guard that AVX2 reached this target |
| `third_party/libfacedetection/CMakeLists.txt` | **Ours** — static lib target, AVX2 on, OpenMP off |

**Ours (`src/`):**

| File | Responsibility |
|---|---|
| `src/subject-frame.h` | The published `SubjectFrame` contract. Nothing else. Header-only, zero dependencies beyond `<cstdint>`. |
| `src/i420-bgr-downscale.h` | Header-only, pure: pick the downscaled size, and convert I420 (BT.709 full range) to packed BGR8 at that size in one pass. |
| `src/subject-detector-fd-record.h` | Header-only, pure: turn one raw libfacedetection result record (`short[142]`) into a normalized `SubjectFrame`. This is the arithmetic the adapter would otherwise hide behind the CNN. |
| `src/subject-detector.h` | The narrow interface `ISubjectDetector` (`frame → box + landmarks`) plus the factory declaration. This is the seam that keeps an OpenCV-DNN fallback a contained swap. |
| `src/subject-detector-fd.cpp` | The only file in the plugin that includes `facedetectcnn.h`. Implements `ISubjectDetector` over `facedetect_cnn()`. |
| `src/subject-schedule.h` | Header-only, pure: round-robin over the monitored roster with an active-speaker boost. One detection per tick, O(1) in roster size. |
| `src/subject-smoothing.h` | Header-only, pure: deadband, rate limit, dropout hold. |
| `src/subject-frame-inbox.h` | Header-only, pure: the single-slot request/deliver handshake between the engine-IPC reader thread and the detector worker. |
| `src/subject-detector-engine.h` / `.cpp` | The worker thread and the public API. Owns the schedule, the inbox, the detector, and one smoother per participant. |

**Modified:**

| File | Change |
|---|---|
| `CMakeLists.txt` | `add_subdirectory(third_party/libfacedetection)`; two new plugin sources; link the vendored lib; six new test registrations. |
| `src/zoom-supersource.cpp` | One `offer_frame` call in `tile_feed_on_frame`; roster/speaker pushes where the feed set changes. |
| `docs/THIRD-PARTY-NOTICES.md` | New file: the BSD-3 attribution notice we are obliged to carry. |
| `CLAUDE.md` | Record the vendoring exception and the detector's threading rule. |

**Tests:**

| Test file | Target / ctest name |
|---|---|
| `tests/facedetect-link-test.cpp` | `CoreVideoFaceDetectLinkTest` / `CoreVideoFaceDetectLink` |
| `tests/subject-detector-bench.cpp` | `CoreVideoSubjectDetectorBenchTest` / `CoreVideoSubjectDetectorBench` |
| `tests/i420-bgr-downscale-test.cpp` | `CoreVideoI420BgrDownscaleTest` / `CoreVideoI420BgrDownscale` |
| `tests/subject-detector-record-test.cpp` | `CoreVideoSubjectDetectorRecordTest` / `CoreVideoSubjectDetectorRecord` |
| `tests/subject-schedule-test.cpp` | `CoreVideoSubjectScheduleTest` / `CoreVideoSubjectSchedule` |
| `tests/subject-smoothing-test.cpp` | `CoreVideoSubjectSmoothingTest` / `CoreVideoSubjectSmoothing` |
| `tests/subject-inbox-test.cpp` | `CoreVideoSubjectInboxTest` / `CoreVideoSubjectInbox` |
| `tests/subject-worker-test.cpp` | `CoreVideoSubjectWorkerTest` / `CoreVideoSubjectWorker` |

**Task order rationale:** Task 1 vendors and proves the library links. Task 2 measures its real cost on this machine *before* anything depends on a tick rate — the spec's ~10 ms figure is an extrapolation from a published 13.09 ms at 320×240 on a 2017 i7-7820X, and every later task's cadence assumption rests on the measurement. Tasks 3–7 are pure headers, each independently testable. Task 8 assembles them and wires the plugin.

---

### Task 1: Vendor libfacedetection

**Files:**
- Create: `third_party/libfacedetection/LICENSE`
- Create: `third_party/libfacedetection/README-COREVIDEO.md`
- Create: `third_party/libfacedetection/src/facedetectcnn.h` (upstream copy)
- Create: `third_party/libfacedetection/src/facedetectcnn.cpp` (upstream copy)
- Create: `third_party/libfacedetection/src/facedetectcnn-model.cpp` (upstream copy)
- Create: `third_party/libfacedetection/src/facedetectcnn-data.cpp` (upstream copy)
- Create: `third_party/libfacedetection/src/facedetection_export.h` (ours)
- Create: `third_party/libfacedetection/corevideo-avx2-assert.cpp` (ours)
- Create: `third_party/libfacedetection/CMakeLists.txt` (ours)
- Create: `docs/THIRD-PARTY-NOTICES.md`
- Create: `tests/facedetect-link-test.cpp`
- Modify: `CMakeLists.txt` (add_subdirectory before the plugin target; test registration in the `BUILD_TESTING` block)
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake target `libfacedetection` (STATIC, `PUBLIC` include dir `third_party/libfacedetection/src`). The C++ entry point it exposes is upstream's:
  `int *facedetect_cnn(unsigned char *result_buffer, unsigned char *bgr_image_data, int width, int height, int step);`
  with `DETECT_BUFFER_SIZE` (0x20000) the required `result_buffer` size. Result layout: `results[0]` is the face count; face `i` is `reinterpret_cast<short *>(results + 1) + 142 * i`, whose fields are `[0]` confidence 0–100, `[1..4]` x, y, w, h in pixels, `[5..14]` five landmark x,y pairs.

- [ ] **Step 1: Fetch upstream and record the exact commit**

```bash
# Run from the repo root. The scratch clone is thrown away; only the four
# source files and the LICENSE are kept.
git ls-remote https://github.com/ShiqiYu/libfacedetection.git HEAD
# ^ Copy the SHA it prints. It goes in README-COREVIDEO.md in Step 3.

git clone --depth 1 https://github.com/ShiqiYu/libfacedetection.git /tmp/lfd
mkdir -p third_party/libfacedetection/src
cp /tmp/lfd/src/facedetectcnn.h        third_party/libfacedetection/src/
cp /tmp/lfd/src/facedetectcnn.cpp      third_party/libfacedetection/src/
cp /tmp/lfd/src/facedetectcnn-model.cpp third_party/libfacedetection/src/
cp /tmp/lfd/src/facedetectcnn-data.cpp  third_party/libfacedetection/src/
cp /tmp/lfd/LICENSE                     third_party/libfacedetection/LICENSE
```

Then confirm the licence really is 3-clause BSD and the data file really is the weights, not a downloader:

```bash
head -5 third_party/libfacedetection/LICENSE
ls -l third_party/libfacedetection/src/
# Expect: LICENSE begins "Copyright (c) 2018-2021, Shiqi Yu ... BSD 3-Clause",
# and facedetectcnn-data.cpp is roughly 400-500 KB.
```

- [ ] **Step 2: Discover the exact SIMD macro spelling upstream uses**

Upstream gates its AVX2 path on a preprocessor macro, and the spelling has differed between releases (`_ENABLE_AVX2` vs `_ENABLE_AVX2_`). Do not guess — read it:

```bash
grep -n "AVX2\|AVX512\|NEON" third_party/libfacedetection/src/facedetectcnn.h
```

Whatever it prints, the CMakeLists in Step 4 defines **both** spellings. Defining a macro the code never tests is harmless; failing to define the one it does test silently drops the whole SIMD path, which is exactly the failure mode this task exists to prevent. Note the spelling you saw in `README-COREVIDEO.md`.

- [ ] **Step 3: Write the export header, the AVX2 guard, and the provenance note**

`third_party/libfacedetection/src/facedetection_export.h`:

```cpp
#define FACEDETECTION_EXPORT
```

`third_party/libfacedetection/corevideo-avx2-assert.cpp`:

```cpp
// Fails the build if AVX2 did not reach the libfacedetection target's compile
// line.
//
// WHY THIS FILE EXISTS. MSVC will happily compile AVX2 intrinsics under the
// default /arch:SSE2: it accepts the intrinsics, emits a slower path, and says
// nothing. The only symptom is a detector that measures several times its
// published cost, which reads as "the library is bad" rather than "our build
// is wrong" -- and the spec calls that out as a day-costing trap. A compile
// error is the cheapest possible early warning, so this file is compiled into
// the same target as the library sources and therefore sees the same flags
// they do.
//
// Guarded on COREVIDEO_FD_EXPECT_AVX2, which CMake defines only on x86-64,
// so an ARM build (where there is no __AVX2__ and NEON is the SIMD path) is
// not broken by it.

#if defined(COREVIDEO_FD_EXPECT_AVX2) && !defined(__AVX2__)
#error "libfacedetection is not being compiled with AVX2 enabled -- /arch:AVX2 (MSVC) or -mavx2 did not reach this target's compile line. Fix third_party/libfacedetection/CMakeLists.txt; do not delete this check."
#endif

// Keeps the translation unit non-empty for linkers that object to one.
namespace corevideo_facedetect_build_guard { const int kAvx2Checked = 1; }
```

`third_party/libfacedetection/README-COREVIDEO.md`:

```markdown
# libfacedetection — vendored into CoreVideo

Upstream: https://github.com/ShiqiYu/libfacedetection
Pinned commit: <PASTE THE SHA FROM `git ls-remote` HERE>
Licence: 3-clause BSD — see `LICENSE` in this directory, reproduced in
`docs/THIRD-PARTY-NOTICES.md`.

## Why this is vendored rather than depended on

This repository had never vendored a library before. The exception is
deliberate and was argued in
`docs/superpowers/specs/2026-09-05-panelist-feedback-design.md`:

- It *is* YuNet — upstream converted the OpenCV Zoo YuNet ONNX model into
  static C arrays, so we get the leading small face model without the runtime
  that normally carries it.
- Zero dependencies and **no external model asset**. Nothing to install,
  path-resolve, ship, or code-sign at runtime; no new DLL in the plugin folder.
- Upstream explicitly sanctions copying the sources into a host project.
- The alternative (`cv::FaceDetectorYN`) is the same model but drags in
  opencv_core + dnn + imgproc. It stays the documented fallback, reachable
  through `ISubjectDetector` in `src/subject-detector.h` without touching any
  consumer.

## Files copied verbatim from upstream `src/`

- `src/facedetectcnn.h`
- `src/facedetectcnn.cpp`
- `src/facedetectcnn-model.cpp`
- `src/facedetectcnn-data.cpp`  (~446 KB: the model weights as static C arrays)

Do not edit these. If they ever need a fix, re-vendor from a newer upstream
commit and update the SHA above.

## Files WE wrote (not upstream)

- `src/facedetection_export.h` — upstream's build generates this; ours is the
  one-line no-op form, because we build the sources directly into the plugin
  rather than as a shared library.
- `corevideo-avx2-assert.cpp` — build-time proof that AVX2 reached the compile
  line. Read its header comment before deleting it.
- `CMakeLists.txt` — our target definition.

## SIMD macro spelling

Upstream gates the AVX2 path on a preprocessor macro whose spelling has varied
across releases. As observed in the pinned commit's `facedetectcnn.h`, it is:
`<PASTE THE SPELLING YOU SAW IN STEP 2>`. Our `CMakeLists.txt` defines both
`_ENABLE_AVX2` and `_ENABLE_AVX2_` so a re-vendor cannot silently drop it.

## OpenMP is deliberately OFF

Upstream offers an OpenMP build. We do not use it: the detector runs one
detection at a time on a single dedicated worker thread
(`src/subject-detector-engine.cpp`), and the scheduling that makes cost O(1)
in participant count is ours, not OpenMP's. Turning `/openmp` on would fight
our own thread for cores during a live show.
```

`docs/THIRD-PARTY-NOTICES.md`:

```markdown
# Third-Party Notices

CoreVideo is distributed under the MIT licence (see `LICENSE`). It also
includes the following third-party software, whose own licence terms apply to
those portions.

## libfacedetection

Vendored under `third_party/libfacedetection/`. Used for on-device face
detection (bounding box plus five landmarks) in the subject detector. No model
asset is downloaded or shipped separately; the weights are compiled in.

Source: https://github.com/ShiqiYu/libfacedetection
Licence: 3-Clause BSD

```
By downloading, copying, installing or using the software you agree to this
license. If you do not agree to this license, do not download, install, copy
or use the software.

                  License Agreement For libfacedetection
                     (3-clause BSD License)

Copyright (c) 2018-2021, Shiqi Yu, all rights reserved.
shiqi.yu@gmail.com

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the names of the copyright holders nor the names of the
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

This software is provided by the copyright holders and contributors "as is"
and any express or implied warranties, including, but not limited to, the
implied warranties of merchantability and fitness for a particular purpose are
disclaimed. In no event shall copyright holders or contributors be liable for
any direct, indirect, incidental, special, exemplary, or consequential damages
(including, but not limited to, procurement of substitute goods or services;
loss of use, data, or profits; or business interruption) however caused and on
any theory of liability, whether in contract, strict liability, or tort
(including negligence or otherwise) arising in any way out of the use of this
software, even if advised of the possibility of such damage.
```
```

**Verify the pasted notice matches the file you actually vendored** — replace
the fenced block above with the literal contents of
`third_party/libfacedetection/LICENSE` if upstream's text differs at the pinned
commit:

```bash
diff <(sed -n '/^By downloading/,/possibility of such damage\.$/p' docs/THIRD-PARTY-NOTICES.md) third_party/libfacedetection/LICENSE
```

- [ ] **Step 4: Write the vendored target's CMakeLists**

`third_party/libfacedetection/CMakeLists.txt`:

```cmake
# Vendored libfacedetection (ShiqiYu), 3-clause BSD.
# Provenance, the list of files we wrote vs copied, and the re-vendoring
# procedure are in README-COREVIDEO.md. The attribution notice we are obliged
# to carry is in docs/THIRD-PARTY-NOTICES.md.
#
# STATIC, not MODULE/SHARED: the plugin links it in. There is deliberately no
# new DLL in the OBS plugin folder and no model asset on disk -- the weights
# are static C arrays inside facedetectcnn-data.cpp.

add_library(libfacedetection STATIC
    src/facedetectcnn.cpp
    src/facedetectcnn-model.cpp
    src/facedetectcnn-data.cpp
    corevideo-avx2-assert.cpp
)

target_include_directories(libfacedetection PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The plugin is a MODULE library, so everything linked into it must be PIC.
set_target_properties(libfacedetection PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # NOMINMAX/WIN32_LEAN_AND_MEAN for the same reason obs-zoom-plugin sets
    # them: windows.h's min/max macros rewrite std::min/std::max into a syntax
    # error.
    target_compile_definitions(libfacedetection PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # /wd4244 /wd4267: upstream narrows freely between int/size_t/float. We do
    # not patch vendored sources, so the warnings are silenced at the target
    # rather than in the files.
    target_compile_options(libfacedetection PRIVATE /wd4244 /wd4267)
endif()

# AVX2. Both macro spellings are defined on purpose -- upstream has used
# _ENABLE_AVX2 and _ENABLE_AVX2_ in different releases, and defining the unused
# one costs nothing while missing the used one silently drops the entire SIMD
# path. See README-COREVIDEO.md.
#
# COREVIDEO_FD_EXPECT_AVX2 arms the compile-time guard in
# corevideo-avx2-assert.cpp, which is what actually proves the flag landed.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|x86|X86")
    target_compile_definitions(libfacedetection PRIVATE
        _ENABLE_AVX2 _ENABLE_AVX2_ COREVIDEO_FD_EXPECT_AVX2)
    if(MSVC)
        target_compile_options(libfacedetection PRIVATE /arch:AVX2)
    else()
        target_compile_options(libfacedetection PRIVATE -mavx2 -mfma)
    endif()
endif()

# DELIBERATELY NOT /openmp. We run exactly one detection at a time on our own
# dedicated worker thread and get O(1)-in-roster-size cost from the schedule in
# src/subject-schedule.h. An OpenMP pool inside the detector would compete with
# OBS's render and encode threads during a live show for no benefit. Upstream
# recommends host-level threading for exactly this reason.
```

- [ ] **Step 5: Write the failing link/smoke test**

`tests/facedetect-link-test.cpp`:

```cpp
// tests/facedetect-link-test.cpp
// Proves the vendored libfacedetection actually links and runs in this build.
//
// It is deliberately not an accuracy test: with no face image in the repo
// there is nothing to assert about detection quality here. What it does
// assert is everything that can silently go wrong at vendoring time -- the
// weights translation unit is present, the export header satisfies the
// declaration, the result buffer contract is honoured, and a flat image
// yields a well-formed (zero-face) result rather than a crash or garbage
// count.

#include "facedetectcnn.h"

#include <cstdint>
#include <iostream>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

int main()
{
    const int w = 320;
    const int h = 180;

    // Flat mid-grey BGR. No face, no edges, nothing for the CNN to latch on.
    std::vector<unsigned char> bgr(static_cast<size_t>(w) * h * 3, 128);
    std::vector<unsigned char> buffer(DETECT_BUFFER_SIZE, 0);

    int *results = facedetect_cnn(buffer.data(), bgr.data(), w, h, w * 3);
    check(results != nullptr, "facedetect_cnn returned a null result pointer");
    if (!results) return 1;

    const int count = *results;
    std::cerr << "flat grey 320x180 -> " << count << " face(s)\n";
    check(count >= 0, "face count must not be negative");
    check(count == 0, "flat grey must not produce a face");

    // Run it a second time on the same buffer: the detector is called once per
    // tick for the whole life of the process, so a one-shot-only library would
    // be a blocking discovery. This catches state left behind in the result
    // buffer.
    results = facedetect_cnn(buffer.data(), bgr.data(), w, h, w * 3);
    check(results != nullptr, "second call returned null");
    check(results && *results == 0, "second call on flat grey must also be 0");

    // A vertical gradient: still no face, but it exercises the convolution
    // path on non-constant data rather than a degenerate all-equal image.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            const unsigned char v = static_cast<unsigned char>((y * 255) / h);
            bgr[i + 0] = v;
            bgr[i + 1] = v;
            bgr[i + 2] = v;
        }
    }
    results = facedetect_cnn(buffer.data(), bgr.data(), w, h, w * 3);
    check(results != nullptr, "gradient call returned null");
    check(results && *results >= 0, "gradient face count must not be negative");

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "libfacedetection links and runs\n";
    return 0;
}
```

- [ ] **Step 6: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoFaceDetectLinkTest`
Expected: FAIL — CMake reports `No rule to make target` / `does not exist`, because the target has not been registered yet.

- [ ] **Step 7: Register the vendored library and the test in the root CMakeLists**

In `CMakeLists.txt`, immediately **before** `if(COREVIDEO_BUILD_PLUGIN)` at line 263:

```cmake
# Vendored libfacedetection (BSD-3). Added unconditionally rather than inside
# the plugin block because the pure-logic tests link it too, and BUILD_TESTING
# can be on with COREVIDEO_BUILD_PLUGIN off (that is how the Linux portability
# job runs). See third_party/libfacedetection/README-COREVIDEO.md.
add_subdirectory(third_party/libfacedetection)
```

Inside `if(COREVIDEO_BUILD_PLUGIN)`, in the existing `target_link_libraries(obs-zoom-plugin PRIVATE ...)` call (around line 310), add `libfacedetection` to the list:

```cmake
    target_link_libraries(obs-zoom-plugin PRIVATE
        OBS::libobs
        OBS::obs-frontend-api
        Qt6::Core
        Qt6::Network
        Qt6::Widgets
        libfacedetection
    )
```

Inside the `if(BUILD_TESTING)` block, next to the other pure tests:

```cmake
    # Vendored libfacedetection: does it link, does it run twice, does a flat
    # image produce a well-formed zero-face result. Not an accuracy test --
    # there is no face image in the repo -- but it catches every way vendoring
    # a 446 KB weights blob can silently go wrong.
    add_executable(CoreVideoFaceDetectLinkTest
        tests/facedetect-link-test.cpp
    )
    target_link_libraries(CoreVideoFaceDetectLinkTest PRIVATE libfacedetection)
    add_test(NAME CoreVideoFaceDetectLink
             COMMAND CoreVideoFaceDetectLinkTest)
```

- [ ] **Step 8: Run the test to verify it passes**

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoFaceDetectLink --output-on-failure
```
Expected: PASS, with `libfacedetection links and runs` on stderr and `flat grey 320x180 -> 0 face(s)`.

- [ ] **Step 9: Prove the AVX2 guard is live (not silently skipped)**

The guard only helps if it is actually armed. Verify by breaking it on purpose:

```bash
# Temporarily comment out the /arch:AVX2 line in
# third_party/libfacedetection/CMakeLists.txt, then:
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --target libfacedetection
```
Expected: the build FAILS with `libfacedetection is not being compiled with AVX2 enabled`. Restore the line, rebuild, confirm it succeeds again. If the build *succeeded* with the flag removed, the guard is not armed — check that `CMAKE_SYSTEM_PROCESSOR` matched and `COREVIDEO_FD_EXPECT_AVX2` is being defined.

- [ ] **Step 10: Record the vendoring exception in CLAUDE.md**

Add to `CLAUDE.md`, under the section describing repository layout / dependencies:

```markdown
### Vendored third-party source (one exception)

`third_party/libfacedetection/` is the only vendored library in this repo. It
is 3-clause BSD, has zero dependencies, and compiles its model weights in as
static C arrays, so it adds no DLL and no runtime asset. Read
`third_party/libfacedetection/README-COREVIDEO.md` before touching it: the
files under `src/` are verbatim upstream copies and must not be edited, the
AVX2 build guard exists because MSVC accepts AVX2 intrinsics under /arch:SSE2
without a word, and OpenMP is off on purpose.

Everything the plugin does with it goes through `ISubjectDetector`
(`src/subject-detector.h`), a one-method interface, so swapping in the
OpenCV-DNN YuNet fallback would touch one file.
```

- [ ] **Step 11: Full test sweep and commit**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release --output-on-failure
```
Expected: N/N green, including the new `CoreVideoFaceDetectLink`.

```bash
git add third_party/libfacedetection docs/THIRD-PARTY-NOTICES.md \
        tests/facedetect-link-test.cpp CMakeLists.txt CLAUDE.md
git commit -m "feat: vendor libfacedetection (BSD-3) with an AVX2 build guard"
```

---

### Task 2: Benchmark the real detection cost on this machine

**Why this is here and not later:** the spec's ~10 ms figure at 320×180 is an **extrapolation** from a published 13.09 ms at 320×240 on a 2017 i7-7820X. It is not a measurement. Every later task's tick rate, and the whole "cost is O(1) in panelist count" argument, rests on the real number. Measure it before anything depends on it.

**Files:**
- Create: `tests/subject-detector-bench.cpp`
- Modify: `CMakeLists.txt` (test registration)
- Modify: `third_party/libfacedetection/README-COREVIDEO.md` (record the result)

**Interfaces:**
- Consumes: the `libfacedetection` CMake target and `facedetect_cnn()` from Task 1.
- Produces: a measured per-detection cost in milliseconds at 320×180 and 320×240 on this machine, and a go/no-go verdict against the 100 ms tick that Task 5 assumes. No source API.

**Go/no-go rule, decided in advance so the measurement cannot be rationalised:**

| Measured mean at 320×180 | Verdict |
|---|---|
| ≤ 25 ms | **GO** at the planned 100 ms tick (10 detections/s ≈ 25% of one core worst case, typically ~10%). Proceed unchanged. |
| 25–50 ms | **GO, but the tick becomes 200 ms** (5 detections/s). Change `SubjectScheduleConfig::tick_ns` in Task 5 to `200000000ull` and say so in that task's commit message. Consumers are human-paced; 5 Hz across the roster is still within the spec's 2–5 fps envelope for the boosted active speaker. |
| > 50 ms | **NO-GO on libfacedetection.** Stop and escalate. The documented fallback is `cv::FaceDetectorYN` behind the same `ISubjectDetector` interface (Task 4), which is why the interface exists. Do not proceed to Task 5 with a >50 ms detector and a 100 ms tick — that is a worker thread pegged at half a core for a preshow tool. |

The test itself fails only above 50 ms (the NO-GO line). The 25 ms line is a human decision recorded in the README, not a build failure, because a CI box is allowed to be slower than the operator's machine.

- [ ] **Step 1: Write the benchmark**

`tests/subject-detector-bench.cpp`:

```cpp
// tests/subject-detector-bench.cpp
// Measures what one libfacedetection detection actually costs on THIS machine.
//
// WHY THIS EXISTS. The design spec budgets ~10 ms per detection at 320x180,
// extrapolated from upstream's published 13.09 ms at 320x240 on a 2017
// i7-7820X. That is an extrapolation, not a measurement, and the entire
// scheduling argument -- one detection per 100 ms tick, cost flat in roster
// size -- rests on it. This test turns the assumption into a number, prints
// it, and fails only past the point where the whole library choice is wrong.
//
// Image CONTENT barely matters to the cost: YuNet is a dense CNN evaluated
// over the whole image, so the per-frame work is fixed by resolution and only
// the (tiny) NMS stage varies with how many candidates survive. A synthetic
// image is therefore a fair timing proxy, and it keeps the repo free of a
// checked-in face photograph.

#include "facedetectcnn.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

// Something with structure at face-ish scales, so the convolution path is not
// running on a degenerate constant image. Deterministic, no rand().
static std::vector<unsigned char> synthetic_bgr(int w, int h)
{
    std::vector<unsigned char> bgr(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            const int checker = ((x / 16) + (y / 16)) & 1;
            const int ramp = (x * 255) / (w > 1 ? w - 1 : 1);
            bgr[i + 0] = static_cast<unsigned char>(checker ? ramp : 255 - ramp);
            bgr[i + 1] = static_cast<unsigned char>((y * 255) / (h > 1 ? h - 1 : 1));
            bgr[i + 2] = static_cast<unsigned char>((ramp + (checker ? 64 : 0)) & 0xFF);
        }
    }
    return bgr;
}

struct BenchResult {
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
};

static BenchResult bench(int w, int h, int iterations)
{
    std::vector<unsigned char> bgr = synthetic_bgr(w, h);
    std::vector<unsigned char> buffer(DETECT_BUFFER_SIZE, 0);

    // Warm up: the first call pays page faults on the 446 KB weight arrays and
    // any one-time setup. Timing that would measure the wrong thing.
    for (int i = 0; i < 3; ++i)
        facedetect_cnn(buffer.data(), bgr.data(), w, h, w * 3);

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        volatile int *r = facedetect_cnn(buffer.data(), bgr.data(), w, h, w * 3);
        (void)r;  // volatile so the call cannot be optimised away
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(samples.begin(), samples.end());
    BenchResult out;
    double total = 0.0;
    for (double s : samples) total += s;
    out.mean_ms = total / static_cast<double>(samples.size());
    out.median_ms = samples[samples.size() / 2];
    out.min_ms = samples.front();
    out.max_ms = samples.back();
    return out;
}

static void report(const char *label, const BenchResult &r)
{
    std::cerr << label
              << "  mean=" << r.mean_ms << " ms"
              << "  median=" << r.median_ms << " ms"
              << "  min=" << r.min_ms << " ms"
              << "  max=" << r.max_ms << " ms\n";
}

int main()
{
    // 320x180 is our real working size: a 16:9 source downscaled to a 320 px
    // long edge (src/i420-bgr-downscale.h).
    const BenchResult ours = bench(320, 180, 40);
    report("320x180 (our working size)", ours);

    // 320x240 is upstream's published benchmark size, so this number is
    // directly comparable to the 13.09 ms quoted for a 2017 i7-7820X and tells
    // us whether this machine is faster or slower than that reference.
    const BenchResult published = bench(320, 240, 40);
    report("320x240 (upstream's published size)", published);

    std::cerr << "\n--- schedule budget ---\n";
    const double per_second_at_100ms = ours.mean_ms * 10.0;
    std::cerr << "at a 100 ms tick: " << per_second_at_100ms
              << " ms of CPU per second = "
              << (per_second_at_100ms / 10.0) << "% of one core\n";
    std::cerr << "at a 200 ms tick: " << (ours.mean_ms * 5.0)
              << " ms of CPU per second = "
              << (ours.mean_ms * 5.0 / 10.0) << "% of one core\n";

    if (ours.mean_ms <= 25.0) {
        std::cerr << "VERDICT: GO at the planned 100 ms tick.\n";
    } else if (ours.mean_ms <= 50.0) {
        std::cerr << "VERDICT: GO, but set SubjectScheduleConfig::tick_ns to "
                     "200000000 (200 ms) in Task 5.\n";
    } else {
        std::cerr << "VERDICT: NO-GO. " << ours.mean_ms
                  << " ms per detection is past the 50 ms ceiling. Stop and "
                     "escalate; the fallback is cv::FaceDetectorYN behind the "
                     "same ISubjectDetector interface.\n";
        return 1;
    }

    // A detection that is wildly variable is as bad as a slow one: the worker
    // is a single thread and a 10x outlier stalls the whole roster's refresh.
    if (ours.max_ms > ours.mean_ms * 6.0 && ours.max_ms > 30.0) {
        std::cerr << "FAIL: worst-case " << ours.max_ms
                  << " ms is more than 6x the mean -- detection cost is not "
                     "stable enough to schedule against.\n";
        return 1;
    }

    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `ctest -C Release -R CoreVideoSubjectDetectorBench --output-on-failure`
Expected: FAIL — `No tests were found!!!`, because the target is not registered yet.

- [ ] **Step 3: Register the benchmark**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`, after the `CoreVideoFaceDetectLinkTest` block from Task 1:

```cmake
    # What one detection actually costs on the machine it is built on. The
    # spec's ~10 ms budget is an extrapolation from upstream's published
    # 13.09 ms at 320x240 on a 2017 i7-7820X; the whole "one detection per
    # 100 ms tick, cost flat in roster size" argument rests on the real number,
    # so it is measured before anything depends on it. Prints the figure every
    # run and fails only past the 50 ms ceiling where the library choice itself
    # is wrong.
    add_executable(CoreVideoSubjectDetectorBenchTest
        tests/subject-detector-bench.cpp
    )
    target_link_libraries(CoreVideoSubjectDetectorBenchTest PRIVATE libfacedetection)
    add_test(NAME CoreVideoSubjectDetectorBench
             COMMAND CoreVideoSubjectDetectorBenchTest)
```

- [ ] **Step 4: Build, run, and read the number**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoSubjectDetectorBench --output-on-failure
```

Expected: PASS, with the per-size timings, the schedule budget, and a VERDICT line on stderr. **Read the VERDICT.** If it says NO-GO, stop here and escalate — do not start Task 3.

Run it three times. Machines under load lie once.

- [ ] **Step 5: Record the measurement**

Append to `third_party/libfacedetection/README-COREVIDEO.md`:

```markdown
## Measured cost (do not replace this with an estimate)

Measured by `tests/subject-detector-bench.cpp` on the maintainer's Windows
box, Release, AVX2 on:

| Size | mean | median | max |
|---|---|---|---|
| 320x180 (our working size) | FILL FROM THE RUN ms | FILL ms | FILL ms |
| 320x240 (upstream's published size) | FILL ms | FILL ms | FILL ms |

Upstream publishes 13.09 ms at 320x240 on a 2017 i7-7820X, which is the
comparison the second row exists for.

Tick rate chosen on this evidence: 100 ms or 200 ms - see
`SubjectScheduleConfig::tick_ns` in `src/subject-schedule.h`.

Re-run the benchmark after any re-vendor or compiler upgrade. If the AVX2
guard in `corevideo-avx2-assert.cpp` were ever bypassed, this is the number
that would move.
```

- [ ] **Step 6: Commit**

```bash
git add tests/subject-detector-bench.cpp CMakeLists.txt third_party/libfacedetection/README-COREVIDEO.md
git commit -m "test: measure real libfacedetection cost and pin the tick-rate go/no-go"
```

---

### Task 3: I420 → downscaled BGR conversion

**Files:**
- Create: `src/i420-bgr-downscale.h`
- Create: `tests/i420-bgr-downscale-test.cpp`
- Modify: `CMakeLists.txt` (test registration)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct BgrImage { std::vector<uint8_t> pixels; int width = 0; int height = 0; };` — packed BGR8, row stride exactly `width * 3`.
  - `constexpr int kSubjectLongEdge = 320;`
  - `void subject_scale_size(int src_w, int src_h, int max_long_edge, int &out_w, int &out_h);`
  - `bool i420_to_bgr_downscale(const uint8_t *i420, size_t i420_len, int src_w, int src_h, int max_long_edge, BgrImage &out);`
  - `void bt709_full_to_bgr(int y, int u, int v, uint8_t &b, uint8_t &g, uint8_t &r);`
  - `uint8_t bgr_clamp_byte(float v);`

- [ ] **Step 1: Write the failing test**

`tests/i420-bgr-downscale-test.cpp`:

```cpp
// tests/i420-bgr-downscale-test.cpp
// The colour convert and the downscale sizing, pinned against known values.
//
// The channel-ORDER assertions are the point. libfacedetection wants BGR, the
// plugin holds I420, and an R/B swap produces a picture that still looks like
// a person to a human reviewing a screenshot while quietly costing the CNN
// accuracy. There is no headless GPU harness in this repo to catch that
// downstream, so it is pinned numerically here.

#include "i420-bgr-downscale.h"

#include <cstdint>
#include <iostream>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static void check_near(int got, int want, int tol, const char *what)
{
    const int d = got > want ? got - want : want - got;
    if (d > tol) {
        std::cerr << "FAIL: " << what << " got " << got << " want " << want
                  << " (+/-" << tol << ")\n";
        ++g_failures;
    }
}

// A flat I420 frame: every luma sample `y`, every chroma pair (u, v).
static std::vector<uint8_t> flat_i420(int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
    const size_t y_len = static_cast<size_t>(w) * h;
    std::vector<uint8_t> f(y_len + y_len / 2);
    for (size_t i = 0; i < y_len; ++i) f[i] = y;
    for (size_t i = 0; i < y_len / 4; ++i) {
        f[y_len + i] = u;
        f[y_len + y_len / 4 + i] = v;
    }
    return f;
}

int main()
{
    // ---- sizing ----------------------------------------------------------
    int ow = 0, oh = 0;

    subject_scale_size(1920, 1080, 320, ow, oh);
    check(ow == 320 && oh == 180, "1920x1080 -> 320x180");

    subject_scale_size(640, 360, 320, ow, oh);
    check(ow == 320 && oh == 180, "640x360 (the tile default) -> 320x180");

    // Portrait: the LONG edge is the one that gets capped.
    subject_scale_size(360, 640, 320, ow, oh);
    check(ow == 180 && oh == 320, "360x640 -> 180x320");

    // Never upscale. A source already smaller than the cap is passed through:
    // inventing pixels cannot add detail for the CNN and only costs time.
    subject_scale_size(160, 120, 320, ow, oh);
    check(ow == 160 && oh == 120, "160x120 must not be upscaled");

    // Both output dimensions must be even and at least 2, so the chroma
    // sub-sampling indices can never run off the end of the U/V planes.
    subject_scale_size(1920, 1078, 320, ow, oh);
    check((ow % 2) == 0 && (oh % 2) == 0, "output dims must be even");
    check(ow >= 2 && oh >= 2, "output dims must be at least 2");

    subject_scale_size(3840, 1080, 320, ow, oh);
    check(ow == 320, "32:9 source caps its long edge at 320");
    check((oh % 2) == 0 && oh >= 2, "32:9 source keeps a legal short edge");

    // ---- the colour convert, by itself -----------------------------------
    uint8_t b = 0, g = 0, r = 0;

    bt709_full_to_bgr(255, 128, 128, b, g, r);
    check(b == 255 && g == 255 && r == 255, "Y=255 neutral chroma is white");

    bt709_full_to_bgr(0, 128, 128, b, g, r);
    check(b == 0 && g == 0 && r == 0, "Y=0 neutral chroma is black");

    bt709_full_to_bgr(128, 128, 128, b, g, r);
    check(b == 128 && g == 128 && r == 128, "Y=128 neutral chroma is mid grey");

    // FULL range, BT.709. These triples are the forward transform of the pure
    // primaries:
    //   red   (255,0,0) -> Y 54,  U 99,  V 255
    //   green (0,255,0) -> Y 182, U 30,  V 12
    //   blue  (0,0,255) -> Y 18,  U 255, V 116
    // Tolerance 3 absorbs the rounding of that forward direction; it is far
    // tighter than any channel swap, which would be off by 200 or more.
    bt709_full_to_bgr(54, 99, 255, b, g, r);
    check_near(r, 254, 3, "red: R channel");
    check_near(g, 0, 3, "red: G channel");
    check_near(b, 0, 3, "red: B channel");

    bt709_full_to_bgr(182, 30, 12, b, g, r);
    check_near(r, 0, 3, "green: R channel");
    check_near(g, 255, 3, "green: G channel");
    check_near(b, 0, 3, "green: B channel");

    bt709_full_to_bgr(18, 255, 116, b, g, r);
    check_near(r, 0, 3, "blue: R channel");
    check_near(g, 0, 3, "blue: G channel");
    check_near(b, 254, 3, "blue: B channel");

    // Out-of-gamut input must CLAMP, not wrap. A wrapped byte turns a bright
    // edge into a black one, which is exactly the kind of artefact a CNN keys
    // on.
    bt709_full_to_bgr(255, 255, 255, b, g, r);
    check(r == 255, "over-range must clamp high, not wrap");
    bt709_full_to_bgr(16, 16, 16, b, g, r);
    check(b == 0 && r == 0, "under-range channels clamp to 0, not wrap to 255");

    // ---- the whole conversion -------------------------------------------
    BgrImage img;

    // A 640x360 all-red frame downscaled to 320x180: every output pixel must
    // be red, and the buffer must be exactly width*height*3 with no padding.
    std::vector<uint8_t> red = flat_i420(640, 360, 54, 99, 255);
    check(i420_to_bgr_downscale(red.data(), red.size(), 640, 360, 320, img),
          "640x360 red frame converts");
    check(img.width == 320 && img.height == 180, "converted size is 320x180");
    check(img.pixels.size() == static_cast<size_t>(320) * 180 * 3,
          "buffer is exactly w*h*3 -- stride must be w*3 with no padding");
    bool all_red = true;
    for (size_t i = 0; i + 2 < img.pixels.size(); i += 3) {
        if (img.pixels[i] > 3 || img.pixels[i + 1] > 3 ||
            img.pixels[i + 2] < 250) {
            all_red = false;
            break;
        }
    }
    check(all_red, "every output pixel of a red frame is B=0 G=0 R=254");

    // Spatial fidelity: a frame whose left half is red and right half is blue
    // must still be red on the left and blue on the right after downscaling.
    // This is what catches an inverted or transposed sample mapping, which a
    // flat frame cannot see.
    const int sw = 640, sh = 360;
    const size_t y_len = static_cast<size_t>(sw) * sh;
    std::vector<uint8_t> split(y_len + y_len / 2);
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            split[static_cast<size_t>(y) * sw + x] =
                (x < sw / 2) ? uint8_t(54) : uint8_t(18);
        }
    }
    for (int cy = 0; cy < sh / 2; ++cy) {
        for (int cx = 0; cx < sw / 2; ++cx) {
            const size_t ci = static_cast<size_t>(cy) * (sw / 2) + cx;
            const bool left = cx < sw / 4;
            split[y_len + ci] = left ? uint8_t(99) : uint8_t(255);
            split[y_len + y_len / 4 + ci] = left ? uint8_t(255) : uint8_t(116);
        }
    }
    check(i420_to_bgr_downscale(split.data(), split.size(), sw, sh, 320, img),
          "split frame converts");
    {
        // Sample well inside each half so the boundary column cannot decide it.
        const size_t left_i =
            (static_cast<size_t>(img.height / 2) * img.width + img.width / 4) * 3;
        const size_t right_i =
            (static_cast<size_t>(img.height / 2) * img.width +
             (img.width * 3) / 4) * 3;
        check_near(img.pixels[left_i + 2], 254, 4, "left half stays red (R)");
        check_near(img.pixels[left_i + 0], 0, 4, "left half stays red (B)");
        check_near(img.pixels[right_i + 0], 254, 4, "right half stays blue (B)");
        check_near(img.pixels[right_i + 2], 0, 4, "right half stays blue (R)");
    }

    // Reuse of one BgrImage across frames of different sizes must resize
    // correctly rather than leave the old dimensions or old pixels behind --
    // the worker owns exactly one of these for its whole life.
    std::vector<uint8_t> small = flat_i420(160, 120, 255, 128, 128);
    check(i420_to_bgr_downscale(small.data(), small.size(), 160, 120, 320, img),
          "smaller frame converts into the reused image");
    check(img.width == 160 && img.height == 120, "reused image resizes down");
    check(img.pixels.size() == static_cast<size_t>(160) * 120 * 3,
          "reused image's buffer resizes down");
    check(img.pixels[0] == 255 && img.pixels[1] == 255 && img.pixels[2] == 255,
          "reused image holds the NEW frame's pixels, not the old ones");

    // ---- refusals --------------------------------------------------------
    check(!i420_to_bgr_downscale(nullptr, 0, 640, 360, 320, img),
          "null input is refused");
    check(!i420_to_bgr_downscale(red.data(), red.size() - 1, 640, 360, 320, img),
          "a buffer shorter than w*h*3/2 is refused rather than read past");
    check(!i420_to_bgr_downscale(red.data(), red.size(), 0, 360, 320, img),
          "zero width is refused");
    check(!i420_to_bgr_downscale(red.data(), red.size(), 641, 360, 320, img),
          "odd source width has no valid I420 chroma layout and is refused");

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "i420-bgr-downscale OK\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoI420BgrDownscaleTest`
Expected: FAIL — CMake reports the target does not exist. Once Step 4 registers it, the first compile fails with `Cannot open include file: 'i420-bgr-downscale.h'`.

- [ ] **Step 3: Write the header**

`src/i420-bgr-downscale.h`:

```cpp
#pragma once

// Turning one I420 frame into the small packed-BGR image the subject detector
// wants, in a single pass, at the downscaled size.
//
// Extracted so it can be tested without libobs, the Zoom SDK or a GPU, the
// same treatment i420-range-expand.h and audio-silence-fade.h get, and for the
// same reason: a wrong colour convert has no loud symptom. It produces a
// picture that still looks like a person to anyone reviewing a screenshot
// while quietly costing the CNN accuracy, and there is no headless GPU harness
// in this repo to catch it downstream.
//
// THREE DECISIONS THIS FILE ENCODES.
//
// 1. BGR, NOT GREYSCALE. libfacedetection is a CNN trained on three-channel
//    BGR input. Feeding it the Y plane replicated into three channels -- the
//    shortcut that works fine for a Haar or HOG detector -- is not valid here.
//    U and V are already sitting in the buffer next to Y, so the convert is
//    cheap; skipping it would be a silent accuracy loss, not a saving.
//
// 2. DOWNSCALE AND CONVERT IN ONE PASS, OUTPUT-DRIVEN. The naive order
//    (convert the whole frame to BGR, then resize) does 1920*1080 = 2.07M
//    colour conversions to throw away 97% of them. This walks the OUTPUT grid,
//    so it does exactly out_w*out_h conversions: 57,600 at 320x180. That is
//    36x less work than the naive order at 1080p, and still 4x less at the
//    360p the tiles wall subscribes at (zoom-supersource.cpp's
//    tile_feed_subscribe).
//
// 3. POINT SAMPLING, NOT AREA AVERAGING. A box filter would be more faithful,
//    but the consumer is a detector looking for a face across tens of pixels,
//    not a display. Nearest-neighbour keeps this an integer-indexed single
//    pass with no accumulation buffer, and the spec is explicit that detection
//    accuracy is not the constraint here -- cost is.
//
// COLOUR SPACE: BT.709, FULL range. Not limited/studio swing. The engine
// requests full range from the SDK and src/i420-range-expand.h normalises the
// frames where the SDK ignores that request, so by the time a frame reaches
// the plugin's buffers it is full range on every path. Using limited-range
// coefficients here would wash out contrast on every frame the detector sees.

#include <cstddef>
#include <cstdint>
#include <vector>

// The long edge every frame is reduced to before detection. ~320 px keeps a
// face in a 1080p (or 360p) source well resolved while capping the CNN's cost,
// which is fixed by resolution. tests/subject-detector-bench.cpp measures what
// one detection at this size actually costs on the machine it is built on.
constexpr int kSubjectLongEdge = 320;

// Packed BGR8. Row stride is exactly width*3 -- libfacedetection takes a
// `step` argument and we always pass width*3, so nothing here may pad rows.
struct BgrImage {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
};

// Clamps to a byte without wrapping. A wrapped byte turns a bright edge into a
// black one, which is exactly the kind of artefact a CNN keys on, so this is
// not a cosmetic detail.
inline uint8_t bgr_clamp_byte(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<uint8_t>(v + 0.5f);
}

// One YUV sample to one BGR pixel. BT.709, full range:
//   R = Y                      + 1.5748 * (V - 128)
//   G = Y - 0.1873 * (U - 128) - 0.4681 * (V - 128)
//   B = Y + 1.8556 * (U - 128)
// (The inverse of the BT.709 luma weights 0.2126 / 0.7152 / 0.0722 with no
// 16-235 scaling, because the input is full range.)
inline void bt709_full_to_bgr(int y, int u, int v, uint8_t &b, uint8_t &g,
                              uint8_t &r)
{
    const float fy = static_cast<float>(y);
    const float cu = static_cast<float>(u) - 128.0f;
    const float cv = static_cast<float>(v) - 128.0f;
    r = bgr_clamp_byte(fy + 1.5748f * cv);
    g = bgr_clamp_byte(fy - 0.1873f * cu - 0.4681f * cv);
    b = bgr_clamp_byte(fy + 1.8556f * cu);
}

// The output size for a source frame: aspect preserved, long edge capped at
// `max_long_edge`, never upscaled, both dimensions forced even and at least 2.
//
// EVEN is load-bearing, not tidiness: the sampler below maps an output pixel
// to a source chroma sample by halving indices, and an odd dimension makes the
// last row or column's chroma index the first sample past the plane.
inline void subject_scale_size(int src_w, int src_h, int max_long_edge,
                               int &out_w, int &out_h)
{
    out_w = 0;
    out_h = 0;
    if (src_w <= 0 || src_h <= 0 || max_long_edge <= 0) return;

    const int long_edge = src_w > src_h ? src_w : src_h;
    if (long_edge <= max_long_edge) {
        out_w = src_w;
        out_h = src_h;
    } else if (src_w >= src_h) {
        out_w = max_long_edge;
        out_h = static_cast<int>(
            (static_cast<int64_t>(src_h) * max_long_edge) / src_w);
    } else {
        out_h = max_long_edge;
        out_w = static_cast<int>(
            (static_cast<int64_t>(src_w) * max_long_edge) / src_h);
    }

    out_w &= ~1;  // round DOWN to even
    out_h &= ~1;
    if (out_w < 2) out_w = 2;
    if (out_h < 2) out_h = 2;
}

// Downscales and converts in one pass. `i420` is tightly packed: Y of
// src_w*src_h, then U of src_w*src_h/4, then V of src_w*src_h/4 -- the layout
// TileFeed::frame carries (see the comment on that member in
// src/zoom-supersource.cpp).
//
// Returns false, leaving `out` untouched, for anything it cannot safely read:
// null, odd source dimensions (no valid I420 chroma layout -- the same
// rejection tile_feed_on_frame already makes), or a buffer shorter than the
// planes it claims. Never reads past `i420_len`.
inline bool i420_to_bgr_downscale(const uint8_t *i420, size_t i420_len,
                                  int src_w, int src_h, int max_long_edge,
                                  BgrImage &out)
{
    if (!i420) return false;
    if (src_w < 2 || src_h < 2) return false;
    if ((src_w & 1) || (src_h & 1)) return false;

    const size_t y_len = static_cast<size_t>(src_w) * static_cast<size_t>(src_h);
    if (i420_len < y_len + y_len / 2) return false;

    int out_w = 0, out_h = 0;
    subject_scale_size(src_w, src_h, max_long_edge, out_w, out_h);
    if (out_w < 2 || out_h < 2) return false;

    const uint8_t *yp = i420;
    const uint8_t *up = i420 + y_len;
    const uint8_t *vp = up + y_len / 4;
    const int cw = src_w / 2;

    out.width = out_w;
    out.height = out_h;
    // resize, not assign: the worker reuses one BgrImage for its whole life,
    // so after the first frame at a given size this allocates nothing.
    out.pixels.resize(static_cast<size_t>(out_w) * out_h * 3);

    for (int oy = 0; oy < out_h; ++oy) {
        // Integer nearest-neighbour: the source row whose centre is closest to
        // this output row's centre.
        int sy = static_cast<int>(
            (static_cast<int64_t>(oy) * 2 + 1) * src_h / (2 * out_h));
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t *yrow = yp + static_cast<size_t>(sy) * src_w;
        const uint8_t *urow = up + static_cast<size_t>(sy / 2) * cw;
        const uint8_t *vrow = vp + static_cast<size_t>(sy / 2) * cw;
        uint8_t *orow = out.pixels.data() + static_cast<size_t>(oy) * out_w * 3;

        for (int ox = 0; ox < out_w; ++ox) {
            int sx = static_cast<int>(
                (static_cast<int64_t>(ox) * 2 + 1) * src_w / (2 * out_w));
            if (sx >= src_w) sx = src_w - 1;
            bt709_full_to_bgr(yrow[sx], urow[sx / 2], vrow[sx / 2],
                              orow[ox * 3 + 0], orow[ox * 3 + 1],
                              orow[ox * 3 + 2]);
        }
    }
    return true;
}
```

- [ ] **Step 4: Register the test**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`:

```cmake
    # I420 (BT.709, FULL range) to packed BGR at the detector's working size.
    # The channel-order assertions are the reason this exists: libfacedetection
    # wants BGR, an R/B swap still looks like a person in a screenshot, and
    # there is no headless GPU harness that would catch the accuracy loss
    # downstream. Header-only, so no extra .cpp.
    add_executable(CoreVideoI420BgrDownscaleTest
        tests/i420-bgr-downscale-test.cpp
    )
    target_include_directories(CoreVideoI420BgrDownscaleTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoI420BgrDownscale
             COMMAND CoreVideoI420BgrDownscaleTest)
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoI420BgrDownscale --output-on-failure
```

Expected: PASS, `i420-bgr-downscale OK`.

- [ ] **Step 6: Commit**

```bash
git add src/i420-bgr-downscale.h tests/i420-bgr-downscale-test.cpp CMakeLists.txt
git commit -m "feat: add I420 to downscaled BGR conversion for the subject detector"
```

---

### Task 4: The `SubjectFrame` contract and the narrow detector interface

**Files:**
- Create: `src/subject-frame.h`
- Create: `src/subject-detector-fd-record.h`
- Create: `src/subject-detector.h`
- Create: `src/subject-detector-fd.cpp`
- Create: `tests/subject-detector-record-test.cpp`
- Modify: `CMakeLists.txt` (plugin source, test registration)

**Interfaces:**
- Consumes: `BgrImage` / `kSubjectLongEdge` from Task 3 (only as documentation of what the caller will pass; the interface itself takes a raw pointer).
- Produces — **this is the published contract two later plans consume; do not change these field names:**

```cpp
struct SubjectFrame {
    bool     found;
    float    box_x, box_y, box_w, box_h;          // normalized 0..1 of source
    float    eye_l_x, eye_l_y, eye_r_x, eye_r_y;  // normalized 0..1
    float    confidence;
    uint64_t detected_ns;                          // source frame timestamp
};
```

Plus:
- `class ISubjectDetector { virtual SubjectFrame detect(const uint8_t *bgr, int width, int height, uint64_t source_ns) = 0; };`
- `std::unique_ptr<ISubjectDetector> make_facedetect_cnn_detector(int min_confidence_pct);`
- `constexpr int kSubjectMinConfidencePct = 60;`
- `SubjectFrame subject_from_fd_record(const short *record, int width, int height, uint64_t source_ns);`
- `const short *subject_best_fd_record(const int *results, int min_confidence_pct);`

- [ ] **Step 1: Write the failing test**

`tests/subject-detector-record-test.cpp`:

```cpp
// tests/subject-detector-record-test.cpp
// The arithmetic between libfacedetection's raw result record and the
// SubjectFrame two other subsystems consume.
//
// WHY THIS IS A SEPARATE, PURE HEADER. Everything interesting about the
// adapter is this arithmetic -- pixel-to-normalized, which of the five
// landmarks are the eyes, which eye is which, clamping a box the CNN pushed
// past the frame edge, and picking the best of several faces. Left inside the
// .cpp that calls the CNN, none of it could be tested without a face
// photograph in the repo and a detector that is guaranteed to find it. Pulled
// out here, all of it is pinnable against hand-built records.
//
// The EYE-ORDER assertion is the one worth reading twice. Upstream emits the
// two eyes in a fixed order which is the SUBJECT's right eye first -- i.e. the
// one on the LEFT of the image. Our field names are eye_l/eye_r and every
// consumer will read them as image-left and image-right (an eyeline midpoint,
// a horizontal-centre check). So the adapter sorts the pair by x and the test
// pins that, rather than trusting a convention that has flipped between
// upstream releases.

#include "subject-detector-fd-record.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static void check_near(float got, float want, float tol, const char *what)
{
    const float d = got > want ? got - want : want - got;
    if (d > tol) {
        std::cerr << "FAIL: " << what << " got " << got << " want " << want
                  << "\n";
        ++g_failures;
    }
}

// Builds one libfacedetection result record: confidence, box, then five
// landmark x,y pairs in upstream's order (eye A, eye B, nose, mouth corner,
// mouth corner). 142 shorts per record is upstream's stride.
static std::vector<short> make_record(short conf, short x, short y, short w,
                                      short h, short e0x, short e0y, short e1x,
                                      short e1y)
{
    std::vector<short> rec(142, 0);
    rec[0] = conf;
    rec[1] = x;
    rec[2] = y;
    rec[3] = w;
    rec[4] = h;
    rec[5] = e0x;
    rec[6] = e0y;
    rec[7] = e1x;
    rec[8] = e1y;
    rec[9] = static_cast<short>((e0x + e1x) / 2);  // nose
    rec[10] = static_cast<short>(e0y + 20);
    rec[11] = e0x;  rec[12] = static_cast<short>(e0y + 40);  // mouth corners
    rec[13] = e1x;  rec[14] = static_cast<short>(e1y + 40);
    return rec;
}

// Packs `records` into the buffer shape facedetect_cnn returns: an int count
// followed by the shorts.
static std::vector<int> make_results(const std::vector<std::vector<short>> &records)
{
    std::vector<int> buf(1 + (records.size() * 142 * sizeof(short)) / sizeof(int) + 2, 0);
    buf[0] = static_cast<int>(records.size());
    short *p = reinterpret_cast<short *>(buf.data() + 1);
    for (size_t i = 0; i < records.size(); ++i)
        std::memcpy(p + 142 * i, records[i].data(), 142 * sizeof(short));
    return buf;
}

int main()
{
    // ---- normalization ---------------------------------------------------
    // A 320x180 image with a face box at (80, 36) size 80x54, eyes at
    // (100, 60) and (140, 62).
    {
        std::vector<short> rec = make_record(92, 80, 36, 80, 54, 100, 60, 140, 62);
        const SubjectFrame f = subject_from_fd_record(rec.data(), 320, 180, 12345);

        check(f.found, "a well-formed record produces found=true");
        check_near(f.box_x, 80.0f / 320.0f, 0.001f, "box_x normalizes by width");
        check_near(f.box_y, 36.0f / 180.0f, 0.001f, "box_y normalizes by height");
        check_near(f.box_w, 80.0f / 320.0f, 0.001f, "box_w normalizes by width");
        check_near(f.box_h, 54.0f / 180.0f, 0.001f, "box_h normalizes by height");
        check_near(f.confidence, 0.92f, 0.001f,
                   "confidence is the record's 0-100 value scaled to 0..1");
        check(f.detected_ns == 12345,
              "detected_ns is the SOURCE frame timestamp, passed through");
    }

    // ---- eye ordering ----------------------------------------------------
    // Same face, but the record lists the image-RIGHT eye first. eye_l must
    // still be the image-left one, or every consumer's eyeline and
    // horizontal-centre arithmetic is mirrored.
    {
        std::vector<short> a = make_record(90, 80, 36, 80, 54, 100, 60, 140, 62);
        std::vector<short> b = make_record(90, 80, 36, 80, 54, 140, 62, 100, 60);
        const SubjectFrame fa = subject_from_fd_record(a.data(), 320, 180, 1);
        const SubjectFrame fb = subject_from_fd_record(b.data(), 320, 180, 1);

        check_near(fa.eye_l_x, 100.0f / 320.0f, 0.001f, "eye_l is the smaller x");
        check_near(fa.eye_r_x, 140.0f / 320.0f, 0.001f, "eye_r is the larger x");
        check_near(fa.eye_l_y, 60.0f / 180.0f, 0.001f, "eye_l_y follows eye_l_x");
        check_near(fa.eye_r_y, 62.0f / 180.0f, 0.001f, "eye_r_y follows eye_r_x");

        check_near(fb.eye_l_x, fa.eye_l_x, 0.0001f,
                   "record order must not change which eye is eye_l");
        check_near(fb.eye_r_x, fa.eye_r_x, 0.0001f,
                   "record order must not change which eye is eye_r");
        check_near(fb.eye_l_y, fa.eye_l_y, 0.0001f,
                   "the y coordinate travels with its own x");
        check_near(fb.eye_r_y, fa.eye_r_y, 0.0001f,
                   "the y coordinate travels with its own x");
    }

    // ---- clamping --------------------------------------------------------
    // The CNN can place a box partly outside the image (a face at the edge).
    // Consumers treat these as fractions of the frame and index into it, so
    // out-of-range values are a bug they would have to defend against
    // individually. Clamp once, here.
    {
        std::vector<short> rec = make_record(80, -20, -10, 400, 250, -5, 5, 340, 8);
        const SubjectFrame f = subject_from_fd_record(rec.data(), 320, 180, 7);
        check(f.box_x >= 0.0f && f.box_y >= 0.0f, "box origin clamps to >= 0");
        check(f.box_x + f.box_w <= 1.0001f,
              "box must not extend past the right edge");
        check(f.box_y + f.box_h <= 1.0001f,
              "box must not extend past the bottom edge");
        check(f.eye_l_x >= 0.0f && f.eye_l_x <= 1.0f, "eye_l_x clamps into 0..1");
        check(f.eye_r_x >= 0.0f && f.eye_r_x <= 1.0f, "eye_r_x clamps into 0..1");
        check(f.eye_l_y >= 0.0f && f.eye_l_y <= 1.0f, "eye_l_y clamps into 0..1");
        check(f.eye_r_y >= 0.0f && f.eye_r_y <= 1.0f, "eye_r_y clamps into 0..1");
    }

    // A degenerate zero-size box is not a subject.
    {
        std::vector<short> rec = make_record(95, 10, 10, 0, 0, 10, 10, 10, 10);
        const SubjectFrame f = subject_from_fd_record(rec.data(), 320, 180, 1);
        check(!f.found, "a zero-area box is not a subject");
    }

    // A null record, or a nonsense image size, yields a clean not-found rather
    // than dividing by zero.
    {
        const SubjectFrame f = subject_from_fd_record(nullptr, 320, 180, 1);
        check(!f.found, "null record is not a subject");
        std::vector<short> rec = make_record(90, 10, 10, 20, 20, 12, 14, 22, 14);
        const SubjectFrame z = subject_from_fd_record(rec.data(), 0, 0, 1);
        check(!z.found, "zero-size image is not a subject");
    }

    // ---- picking the best record ----------------------------------------
    {
        std::vector<std::vector<short>> recs;
        recs.push_back(make_record(65, 10, 10, 20, 20, 12, 14, 22, 14));
        recs.push_back(make_record(88, 100, 40, 60, 60, 112, 60, 148, 61));
        recs.push_back(make_record(71, 200, 20, 30, 30, 205, 28, 222, 29));
        std::vector<int> buf = make_results(recs);

        const short *best = subject_best_fd_record(buf.data(), 60);
        check(best != nullptr, "a record above the threshold is found");
        check(best && best[0] == 88, "the HIGHEST-confidence face wins");

        // Raising the bar past every candidate must yield nothing, not the
        // least-bad one. A preshow framing tool acting on a 40%-confidence
        // detection would tell a correctly framed panelist to move.
        check(subject_best_fd_record(buf.data(), 95) == nullptr,
              "nothing clears a 95 threshold");

        // Zero faces, and a null pointer, are both ordinary.
        std::vector<int> empty = make_results({});
        check(subject_best_fd_record(empty.data(), 60) == nullptr,
              "zero faces yields no record");
        check(subject_best_fd_record(nullptr, 60) == nullptr,
              "a null results pointer yields no record");
    }

    // ---- the contract itself --------------------------------------------
    {
        // A default SubjectFrame must be a safe "no subject", because that is
        // what every consumer gets before the first detection lands.
        const SubjectFrame f{};
        check(!f.found, "a default-constructed SubjectFrame is not found");
        check(f.confidence == 0.0f && f.detected_ns == 0,
              "a default-constructed SubjectFrame is zeroed");
    }

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "subject-detector-record OK\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoSubjectDetectorRecordTest`
Expected: FAIL — target does not exist; once registered, `Cannot open include file: 'subject-detector-fd-record.h'`.

- [ ] **Step 3: Write the contract type**

`src/subject-frame.h`:

```cpp
#pragma once

// The subject detector's published result type.
//
// THIS IS A CONTRACT, NOT AN IMPLEMENTATION DETAIL. Two separate subsystems
// consume it -- Tiles auto-framing and the return-feed framing overlay -- and
// they are built by different people at different times against
// docs/superpowers/specs/2026-09-05-panelist-feedback-design.md. The field
// names and their meanings are fixed there. Renaming, reordering or
// repurposing a field breaks a plan that has not been written yet, so do not.
//
// ALL COORDINATES ARE NORMALIZED 0..1 OF THE SOURCE FRAME, never pixels of the
// downscaled image the detector actually ran on. That is deliberate: the
// detector runs at ~320 px long edge and the consumers work in tile space, in
// canvas space, and in overlay space -- none of which is the detector's
// resolution. Normalizing at the source removes the one conversion everybody
// would otherwise have to get right independently, and it means changing the
// detection resolution never touches a consumer.
//
// eye_l / eye_r are IMAGE-left and IMAGE-right, not the subject's own left and
// right. The adapter sorts them by x so this holds regardless of what order
// the underlying model emits its landmarks in (see
// src/subject-detector-fd-record.h).
//
// `found == false` means "no subject right now" and every other field is
// meaningless -- but note that a SubjectFrame reaching a consumer has been
// through SubjectSmoother (src/subject-smoothing.h), which HOLDS the last good
// result across brief dropouts. So `found == false` from the engine already
// means "gone for longer than the hold period", not "missed one detection".
//
// `detected_ns` is the timestamp of the SOURCE FRAME the detection ran on, not
// the time the detection finished. A consumer can age the result with it: a
// held-through-dropout frame keeps the timestamp of the last real detection,
// which is exactly what "how stale is this?" needs.

#include <cstdint>

struct SubjectFrame {
    bool     found = false;
    float    box_x = 0.0f, box_y = 0.0f, box_w = 0.0f, box_h = 0.0f;
    float    eye_l_x = 0.0f, eye_l_y = 0.0f, eye_r_x = 0.0f, eye_r_y = 0.0f;
    float    confidence = 0.0f;
    uint64_t detected_ns = 0;
};
```

- [ ] **Step 4: Write the record arithmetic**

`src/subject-detector-fd-record.h`:

```cpp
#pragma once

// Turning one libfacedetection result record into a SubjectFrame.
//
// Extracted from the adapter that calls the CNN so it can be tested at all.
// Everything that can actually be WRONG about the adapter is here --
// pixel-to-normalized, which landmarks are the eyes, which eye is which,
// clamping a box the CNN pushed past the frame edge, and picking the best of
// several faces -- and none of it could be exercised inside the .cpp without a
// face photograph in the repo and a detector guaranteed to find it. This
// header needs neither. It also has no include of facedetectcnn.h, on purpose:
// the record layout is described below and repeated nowhere else, so the pure
// test does not drag the 446 KB weights blob into its link.
//
// UPSTREAM'S RESULT LAYOUT (facedetect_cnn's return value):
//   results[0]                                   -> face count (int)
//   reinterpret_cast<short*>(results + 1) + 142*i -> face i's record
//     [0]      confidence, 0..100
//     [1..4]   x, y, w, h in PIXELS of the image passed in
//     [5..14]  five landmark x,y pairs: two eyes, nose tip, two mouth corners
//
// THE EYE-ORDER RULE. Upstream emits the eyes in a fixed order that is the
// SUBJECT's right eye first, i.e. the one on the LEFT of the image -- and that
// convention has moved between releases of the model this was converted from.
// Our field names are eye_l/eye_r and every consumer reads them as image-left
// and image-right (eyeline midpoint, horizontal-centre check). So we do not
// trust the order: we sort the pair by x. It costs one compare and makes a
// re-vendor unable to silently mirror everybody's framing arithmetic.

#include "subject-frame.h"

#include <cstdint>

// Stride between consecutive face records, in shorts. Upstream's constant.
constexpr int kFdRecordStride = 142;

// Below this the detection is not acted on. A preshow framing tool that told a
// correctly framed panelist to move because of a 40%-confidence ghost is worse
// than one that says nothing, so the bar is deliberately not at the floor.
constexpr int kSubjectMinConfidencePct = 60;

inline float subject_clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// The highest-confidence record at or above `min_confidence_pct`, or nullptr.
// Highest, not first: upstream's ordering is not a ranking we want to depend
// on, and with one panelist per frame the runner-up is usually a background
// artefact.
inline const short *subject_best_fd_record(const int *results,
                                           int min_confidence_pct)
{
    if (!results) return nullptr;
    const int count = *results;
    if (count <= 0) return nullptr;

    const short *records = reinterpret_cast<const short *>(results + 1);
    const short *best = nullptr;
    int best_conf = min_confidence_pct - 1;
    for (int i = 0; i < count; ++i) {
        const short *rec = records + kFdRecordStride * i;
        if (rec[0] > best_conf) {
            best_conf = rec[0];
            best = rec;
        }
    }
    return best;
}

// One record -> one SubjectFrame, normalized to 0..1 of the image the
// detection ran on. `source_ns` is the timestamp of the frame, carried
// through untouched.
//
// Returns a default (not-found) SubjectFrame for anything unusable: a null
// record, a nonsense image size, or a degenerate zero-area box.
inline SubjectFrame subject_from_fd_record(const short *record, int width,
                                           int height, uint64_t source_ns)
{
    SubjectFrame out{};
    if (!record || width <= 0 || height <= 0) return out;

    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);

    float x = static_cast<float>(record[1]);
    float y = static_cast<float>(record[2]);
    float w = static_cast<float>(record[3]);
    float h = static_cast<float>(record[4]);
    if (w <= 0.0f || h <= 0.0f) return out;

    // Clamp in PIXELS before normalizing, so a box the CNN pushed off the edge
    // shrinks to the visible part instead of keeping its width and sliding.
    // Consumers index into frames with these fractions; letting one exceed 1.0
    // would make every consumer defend against it separately.
    if (x < 0.0f) { w += x; x = 0.0f; }
    if (y < 0.0f) { h += y; y = 0.0f; }
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (w <= 0.0f || h <= 0.0f) return out;

    // Eyes: landmarks 0 and 1, sorted by x so eye_l is always the image-left
    // one. See the header comment -- this is not tidiness, it is what keeps a
    // re-vendor from mirroring every consumer's framing arithmetic.
    float e0x = static_cast<float>(record[5]);
    float e0y = static_cast<float>(record[6]);
    float e1x = static_cast<float>(record[7]);
    float e1y = static_cast<float>(record[8]);
    if (e1x < e0x) {
        const float tx = e0x; e0x = e1x; e1x = tx;
        const float ty = e0y; e0y = e1y; e1y = ty;
    }

    out.found = true;
    out.box_x = subject_clamp01(x / fw);
    out.box_y = subject_clamp01(y / fh);
    out.box_w = subject_clamp01(w / fw);
    out.box_h = subject_clamp01(h / fh);
    out.eye_l_x = subject_clamp01(e0x / fw);
    out.eye_l_y = subject_clamp01(e0y / fh);
    out.eye_r_x = subject_clamp01(e1x / fw);
    out.eye_r_y = subject_clamp01(e1y / fh);
    out.confidence = subject_clamp01(static_cast<float>(record[0]) / 100.0f);
    out.detected_ns = source_ns;
    return out;
}
```

- [ ] **Step 5: Write the narrow interface**

`src/subject-detector.h`:

```cpp
#pragma once

// The entire perception surface of this feature: one downscaled BGR image in,
// one SubjectFrame out.
//
// WHY IT IS THIS NARROW. The spec commits to libfacedetection but keeps
// cv::FaceDetectorYN (the same YuNet model, with an OpenCV dependency) as a
// documented fallback if local benchmarking disappoints. That fallback is only
// cheap if the swap is contained -- one interface, one factory, no consumer
// touched -- so the interface exists from day one rather than being extracted
// later under time pressure. Nothing above this line may learn that the
// detector is a CNN, has weights, or has a result-buffer contract.
//
// The interface is deliberately not thread-safe and does not need to be: it is
// owned and called by exactly one thread, the detector worker in
// src/subject-detector-engine.cpp. Implementations keep a scratch buffer
// across calls precisely because there is only one caller.

#include "subject-frame.h"

#include <cstdint>
#include <memory>

class ISubjectDetector {
public:
    virtual ~ISubjectDetector() = default;

    // `bgr` is packed BGR8, width*height*3 bytes, row stride exactly width*3
    // -- what src/i420-bgr-downscale.h produces. `source_ns` is the timestamp
    // of the frame the pixels came from and is copied into the result's
    // detected_ns.
    //
    // Returns the highest-confidence face found, normalized to 0..1 of the
    // image passed in. `found == false` when nothing cleared the detector's
    // confidence threshold; the other fields are then meaningless.
    virtual SubjectFrame detect(const uint8_t *bgr, int width, int height,
                                uint64_t source_ns) = 0;
};

// The vendored libfacedetection implementation.
// `min_confidence_pct` is on the library's own 0-100 scale; pass
// kSubjectMinConfidencePct (src/subject-detector-fd-record.h) unless a caller
// has a specific reason.
std::unique_ptr<ISubjectDetector> make_facedetect_cnn_detector(
    int min_confidence_pct);
```

- [ ] **Step 6: Write the adapter**

`src/subject-detector-fd.cpp`:

```cpp
// The libfacedetection implementation of ISubjectDetector.
//
// This is the ONLY file in the plugin that includes facedetectcnn.h. Keeping
// it that way is what makes the documented cv::FaceDetectorYN fallback a
// one-file swap -- see the header comment on src/subject-detector.h.
//
// All the arithmetic lives in src/subject-detector-fd-record.h so it can be
// unit-tested without a face photograph; what remains here is the call itself
// and the scratch buffer it needs.

#include "subject-detector.h"
#include "subject-detector-fd-record.h"

#include "facedetectcnn.h"

#include <vector>

namespace {

class FaceDetectCnnDetector : public ISubjectDetector {
public:
    explicit FaceDetectCnnDetector(int min_confidence_pct)
        : m_min_confidence_pct(min_confidence_pct),
          m_buffer(DETECT_BUFFER_SIZE, 0)
    {
    }

    SubjectFrame detect(const uint8_t *bgr, int width, int height,
                        uint64_t source_ns) override
    {
        // Anything smaller than this cannot hold a face at the scales the
        // model was trained for, and passing a degenerate image into the CNN
        // is not worth finding out about the hard way.
        if (!bgr || width < 32 || height < 32) return SubjectFrame{};

        // The buffer is a member, allocated once: this runs 5-10 times a
        // second for the life of the process, and DETECT_BUFFER_SIZE is 128 KB.
        //
        // The const_cast is upstream's signature, not a lie about ownership --
        // facedetect_cnn takes `unsigned char *` for its input image and does
        // not write to it.
        int *results = facedetect_cnn(
            m_buffer.data(), const_cast<unsigned char *>(bgr), width, height,
            width * 3);

        const short *best = subject_best_fd_record(results, m_min_confidence_pct);
        return subject_from_fd_record(best, width, height, source_ns);
    }

private:
    int m_min_confidence_pct;
    std::vector<unsigned char> m_buffer;
};

}  // namespace

std::unique_ptr<ISubjectDetector> make_facedetect_cnn_detector(
    int min_confidence_pct)
{
    return std::unique_ptr<ISubjectDetector>(
        new FaceDetectCnnDetector(min_confidence_pct));
}
```

- [ ] **Step 7: Register the source and the test**

In `CMakeLists.txt`, inside `add_library(obs-zoom-plugin MODULE ...)`, after `src/zoom-tile-grid.cpp`:

```cmake
        src/subject-detector-fd.cpp
```

Inside `if(BUILD_TESTING)`:

```cmake
    # The arithmetic between libfacedetection's raw records and the
    # SubjectFrame two other subsystems consume: normalization, eye ordering,
    # edge clamping, best-of-N. Pure -- it deliberately does NOT link
    # libfacedetection, so it needs no face photograph and no weights blob to
    # pin the part of the adapter that can actually be wrong.
    add_executable(CoreVideoSubjectDetectorRecordTest
        tests/subject-detector-record-test.cpp
    )
    target_include_directories(CoreVideoSubjectDetectorRecordTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSubjectDetectorRecord
             COMMAND CoreVideoSubjectDetectorRecordTest)
```

- [ ] **Step 8: Run the test to verify it passes**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoSubjectDetectorRecord --output-on-failure
```

Expected: PASS, `subject-detector-record OK`. The plugin must also still link — `src/subject-detector-fd.cpp` is now in it, so a missing `libfacedetection` link from Task 1 shows up here.

- [ ] **Step 9: Commit**

```bash
git add src/subject-frame.h src/subject-detector-fd-record.h src/subject-detector.h src/subject-detector-fd.cpp tests/subject-detector-record-test.cpp CMakeLists.txt
git commit -m "feat: add the SubjectFrame contract and the narrow subject-detector interface"
```

---

### Task 5: Round-robin schedule with an active-speaker boost

**Files:**
- Create: `src/subject-schedule.h`
- Create: `tests/subject-schedule-test.cpp`
- Modify: `CMakeLists.txt` (test registration)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct SubjectScheduleConfig { uint64_t tick_ns; int speaker_every; };`
  - `class SubjectSchedule` with `set_roster(const std::vector<uint32_t> &)`, `set_active_speaker(uint32_t)`, `uint32_t next(uint64_t now_ns)`, `uint64_t issued() const`, `size_t roster_size() const`.

**If Task 2's benchmark returned the 25–50 ms verdict**, set `tick_ns` to `200000000ull` instead of `100000000ull` in the header below, and say so in this task's commit message. Nothing else in the task changes.

- [ ] **Step 1: Write the failing test**

`tests/subject-schedule-test.cpp`:

```cpp
// tests/subject-schedule-test.cpp
// The scheduling property the whole feature's cost argument rests on: one
// detection per tick, no matter how many people are in the room.
//
// The 2-to-20 sweep is the load-bearing assertion. The naive design -- every
// monitored participant detected at 5 fps -- costs 0.5 of a core at ten
// panelists and grows from there. This schedule issues a FIXED number of
// detections per second and cycles who gets them, so a 24-person Zoom Events
// panel costs exactly what a 2-person mic check costs. A regression here would
// not fail anything else: it would just quietly make the preshow tool a CPU
// hog on the biggest shows, which are the ones that can least afford it.

#include "subject-schedule.h"

#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static std::vector<uint32_t> roster_of(int n)
{
    std::vector<uint32_t> ids;
    for (int i = 0; i < n; ++i) ids.push_back(static_cast<uint32_t>(1000 + i));
    return ids;
}

int main()
{
    const SubjectScheduleConfig cfg{};  // 100 ms tick, speaker every 3rd slot

    // ---- the O(1) property ----------------------------------------------
    // Drive a simulated clock through one second in 10 ms steps for every
    // roster size from 2 to 20, and count the detections issued. It must be
    // the same number every time.
    {
        int expected = -1;
        for (int n = 2; n <= 20; ++n) {
            SubjectSchedule sched(cfg);
            sched.set_roster(roster_of(n));
            sched.set_active_speaker(1000);

            int issued = 0;
            for (uint64_t t = 0; t < 1000000000ull; t += 10000000ull) {
                if (sched.next(t) != 0) ++issued;
            }
            if (expected < 0) expected = issued;
            if (issued != expected) {
                std::cerr << "FAIL: roster of " << n << " issued " << issued
                          << " detections in one second, roster of 2 issued "
                          << expected << " -- cost is NOT O(1) in participant "
                             "count\n";
                ++g_failures;
            }
        }
        // At a 100 ms tick, one second of 10 ms steps is 10 detections.
        check(expected == 10,
              "a 100 ms tick issues exactly 10 detections per second");
        std::cerr << "detections per second, every roster size 2..20: "
                  << expected << "\n";
    }

    // A tick is a floor, not a schedule the caller has to hit exactly. Polling
    // faster than the tick must not issue more work -- the worker loop wakes
    // every few milliseconds and asks on every wakeup.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(5));
        int issued = 0;
        for (uint64_t t = 0; t < 1000000000ull; t += 1000000ull) {  // 1 ms
            if (sched.next(t) != 0) ++issued;
        }
        check(issued == 10,
              "polling at 1 ms must still issue only 10 detections per second");
    }

    // ---- round-robin coverage -------------------------------------------
    // Every participant must be visited. A schedule that boosted the speaker
    // so hard that nobody else was ever detected would pass the O(1) test and
    // be useless.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(8));
        sched.set_active_speaker(1000);

        std::set<uint32_t> seen;
        // 8 people, and roughly a third of slots go to the speaker, so allow
        // generous time: 60 ticks is 6 seconds.
        for (uint64_t t = 0; t < 6000000000ull; t += 100000000ull) {
            const uint32_t id = sched.next(t);
            if (id != 0) seen.insert(id);
        }
        check(seen.size() == 8,
              "every participant is detected within a few seconds");
    }

    // ---- the active-speaker boost ---------------------------------------
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(10));
        sched.set_active_speaker(1005);

        int speaker_hits = 0;
        int total = 0;
        for (uint64_t t = 0; t < 6000000000ull; t += 100000000ull) {
            const uint32_t id = sched.next(t);
            if (id == 0) continue;
            ++total;
            if (id == 1005) ++speaker_hits;
        }
        // Without a boost, one of ten participants gets a tenth of the slots.
        // With it, the person actually being mic-checked gets far more --
        // enough that their framing advice refreshes in under a second rather
        // than once per full roster sweep.
        check(speaker_hits * 4 > total,
              "the active speaker gets substantially more than a fair share");
        check(speaker_hits < total,
              "the boost must not starve the rest of the roster");
        std::cerr << "speaker got " << speaker_hits << " of " << total
                  << " slots in a 10-person roster\n";
    }

    // A speaker who is not in the monitored roster is ignored, not scheduled.
    // The directed active speaker can be someone Tiles is not showing.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(3));  // 1000, 1001, 1002
        sched.set_active_speaker(9999);

        for (uint64_t t = 0; t < 3000000000ull; t += 100000000ull) {
            const uint32_t id = sched.next(t);
            if (id != 0 && id == 9999) {
                std::cerr << "FAIL: scheduled a speaker who is not in the "
                             "monitored roster\n";
                ++g_failures;
                break;
            }
        }
    }

    // Speaker 0 means "nobody is speaking" -- the value active_speaker_id()
    // returns with no one on air. It must degrade to plain round robin.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(4));
        sched.set_active_speaker(0);

        std::set<uint32_t> seen;
        int issued = 0;
        for (uint64_t t = 0; t < 1000000000ull; t += 100000000ull) {
            const uint32_t id = sched.next(t);
            if (id != 0) { ++issued; seen.insert(id); }
        }
        check(issued == 10, "no speaker still issues the full tick rate");
        check(seen.size() == 4, "no speaker still visits everybody");
    }

    // ---- roster churn ----------------------------------------------------
    // The roster shrinking under the cursor must not skip, repeat forever, or
    // index out of range. Participants leave mid-show constantly.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(12));
        uint64_t t = 0;
        for (int i = 0; i < 8; ++i, t += 100000000ull) sched.next(t);

        sched.set_roster(roster_of(3));  // 9 people leave
        std::set<uint32_t> seen;
        for (int i = 0; i < 20; ++i, t += 100000000ull) {
            const uint32_t id = sched.next(t);
            if (id != 0) seen.insert(id);
        }
        check(seen.size() == 3, "after a shrink, exactly the survivors cycle");
        check(seen.count(1005) == 0, "a departed participant is never issued");
    }

    // An empty roster issues nothing at all, forever, and does not wedge.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster({});
        sched.set_active_speaker(1000);
        int issued = 0;
        for (uint64_t t = 0; t < 2000000000ull; t += 50000000ull) {
            if (sched.next(t) != 0) ++issued;
        }
        check(issued == 0, "an empty roster issues nothing");

        // ...and recovers the moment someone appears, with no dead interval.
        sched.set_roster(roster_of(2));
        check(sched.next(2100000000ull) != 0,
              "the schedule resumes as soon as the roster is non-empty");
    }

    // Duplicate ids in the roster must not double a participant's share --
    // a caller assembling the list from two sources is an easy mistake and the
    // consequence (uneven refresh) would be invisible.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster({1001, 1002, 1001, 1003, 1002});
        check(sched.roster_size() == 3, "duplicate ids are collapsed");
    }

    // Id 0 is the "nobody" sentinel and must never enter the roster, or
    // next() could return it and the caller would read it as "not due yet".
    {
        SubjectSchedule sched(cfg);
        sched.set_roster({0, 1001, 0, 1002});
        check(sched.roster_size() == 2, "id 0 is rejected from the roster");
    }

    // The clock going backwards (a caller passing a non-monotonic value) must
    // not issue a burst or wedge the schedule.
    {
        SubjectSchedule sched(cfg);
        sched.set_roster(roster_of(4));
        sched.next(5000000000ull);
        check(sched.next(1000000000ull) == 0,
              "a backwards clock does not issue immediately");
        check(sched.next(5100000000ull) != 0,
              "the schedule recovers on the next legitimate tick");
    }

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "subject-schedule OK\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoSubjectScheduleTest`
Expected: FAIL — target does not exist; once registered, `Cannot open include file: 'subject-schedule.h'`.

- [ ] **Step 3: Write the header**

`src/subject-schedule.h`:

```cpp
#pragma once

// Which participant the detector looks at next, and when.
//
// Extracted so it can be tested without a thread, a clock, or a detector, the
// same treatment media-event-queue.h gets, and for a related reason: the only
// symptom of a regression is a CPU cost that grows with the size of the show.
// That is invisible on a developer's two-person test meeting and expensive on
// a 24-person Zoom Events panel -- the exact inversion that makes it worth a
// test rather than a comment.
//
// THE PROPERTY THIS EXISTS TO GUARANTEE: cost is O(1) in participant count.
//
// The naive design is to detect every monitored participant at 2-5 fps. At ten
// panelists and ~10 ms per detection that is ~0.5 of a core, and it grows
// linearly -- so the biggest shows, which have the least headroom, pay the
// most. We do not need it. Every consumer of this data is slow by design:
// framing advice is read by a human who reacts in seconds, and Tiles
// auto-framing is deliberately hysteretic so tiles do not twitch. So the
// schedule issues ONE detection per tick and cycles who gets it. Ten
// detections a second is roughly a tenth of a core whether the room holds two
// people or twenty-four; the ceiling is fixed by the tick, not by the roster.
//
// THE ACTIVE-SPEAKER BOOST. Plain round robin over 20 people at 10 Hz refreshes
// any one person every 2 seconds, which is too slow for the one person
// actually being mic-checked. Every Nth slot therefore goes to the active
// speaker -- ZoomEngineClient::active_speaker_id(), the DIRECTED id after hold
// and dwell, not the raw one. The boost consumes a slot without advancing the
// round-robin cursor, so it steals refresh rate from everyone else but cannot
// starve anyone: the cursor still walks the whole roster.
//
// EVERY LOOKUP HERE IS O(1) OR O(log n) ON A COLD PATH. `next()` does one
// hash-set lookup and one vector index; only set_roster(), called when the
// monitored set actually changes, is linear.
//
// This class is NOT thread-safe. It is owned and driven by one thread, the
// detector worker in src/subject-detector-engine.cpp, which also serializes
// the roster and speaker updates onto it.

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

struct SubjectScheduleConfig {
    // One detection per tick. 100 ms = 10 detections/second across the whole
    // roster, which is inside the spec's 2-5 fps envelope for the boosted
    // active speaker and comfortably above what a human-paced consumer needs.
    //
    // THIS NUMBER IS EVIDENCE-BACKED, NOT A GUESS: see the measured cost table
    // in third_party/libfacedetection/README-COREVIDEO.md, produced by
    // tests/subject-detector-bench.cpp. If the measurement moves, move this.
    uint64_t tick_ns = 100000000ull;

    // Every Nth issued slot goes to the active speaker. 3 gives the person
    // being checked roughly a third of all detections -- a refresh every
    // ~300 ms -- while leaving two thirds for the round robin.
    int speaker_every = 3;
};

class SubjectSchedule {
public:
    explicit SubjectSchedule(SubjectScheduleConfig cfg = {}) : m_cfg(cfg) {}

    // The participants worth detecting -- in practice the ones actually being
    // shown, not the whole meeting. Duplicates and the id-0 sentinel are
    // dropped. Linear, and called only when the monitored set changes.
    void set_roster(const std::vector<uint32_t> &ids)
    {
        m_roster.clear();
        m_present.clear();
        for (uint32_t id : ids) {
            if (id == 0) continue;              // 0 is next()'s "not due" value
            if (!m_present.insert(id).second) continue;  // already have it
            m_roster.push_back(id);
        }
        if (m_roster.empty()) m_cursor = 0;
        else if (m_cursor >= m_roster.size()) m_cursor = 0;
    }

    // ZoomEngineClient::active_speaker_id() -- the DIRECTED id, post hold and
    // dwell. 0 means nobody, which degrades cleanly to plain round robin.
    // SpeakerDirector is poll-only, so the worker polls this in; there is no
    // observer list to subscribe to.
    void set_active_speaker(uint32_t id) { m_speaker = id; }

    // The participant to detect now, or 0 when the tick has not elapsed.
    //
    // Safe to call as often as the caller likes: the tick is a floor, so a
    // worker polling at 5 ms issues exactly as much work as one polling at
    // 100 ms. A backwards clock simply does not fire until now_ns passes the
    // stored deadline again, which is the conservative direction -- it costs
    // one skipped detection, never a burst.
    uint32_t next(uint64_t now_ns)
    {
        if (m_roster.empty()) return 0;
        if (m_started && now_ns - m_last_ns < m_cfg.tick_ns &&
            now_ns >= m_last_ns) {
            return 0;
        }
        if (m_started && now_ns < m_last_ns) {
            // Clock went backwards. Re-anchor and wait out one full tick
            // rather than firing on every call until it catches up.
            m_last_ns = now_ns;
            return 0;
        }
        m_started = true;
        m_last_ns = now_ns;
        ++m_issued;

        if (m_speaker != 0 && m_cfg.speaker_every > 0 &&
            (m_issued % static_cast<uint64_t>(m_cfg.speaker_every)) == 0 &&
            m_present.count(m_speaker) != 0) {
            // Deliberately does NOT advance the cursor: the boost borrows a
            // slot, it does not consume anyone's turn.
            return m_speaker;
        }

        const uint32_t id = m_roster[m_cursor];
        m_cursor = (m_cursor + 1) % m_roster.size();
        return id;
    }

    uint64_t issued() const { return m_issued; }
    size_t roster_size() const { return m_roster.size(); }

private:
    SubjectScheduleConfig m_cfg;
    std::vector<uint32_t> m_roster;
    std::unordered_set<uint32_t> m_present;  // O(1) "is the speaker monitored?"
    size_t m_cursor = 0;
    uint32_t m_speaker = 0;
    uint64_t m_last_ns = 0;
    uint64_t m_issued = 0;
    bool m_started = false;
};
```

- [ ] **Step 4: Register the test**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`:

```cmake
    # One detection per tick, cycled across the roster with a boost for the
    # active speaker. The 2-to-20 sweep is the point: detection cost must not
    # grow with the size of the show, and nothing else in the suite would fail
    # if it started to -- the tool would just become a CPU hog on exactly the
    # biggest panels. Header-only, so no extra .cpp.
    add_executable(CoreVideoSubjectScheduleTest
        tests/subject-schedule-test.cpp
    )
    target_include_directories(CoreVideoSubjectScheduleTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSubjectSchedule
             COMMAND CoreVideoSubjectScheduleTest)
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoSubjectSchedule --output-on-failure
```

Expected: PASS, with `detections per second, every roster size 2..20: 10` and the speaker share printed.

- [ ] **Step 6: Commit**

```bash
git add src/subject-schedule.h tests/subject-schedule-test.cpp CMakeLists.txt
git commit -m "feat: add the O(1) round-robin subject-detection schedule"
```

(If Task 2's benchmark forced the 200 ms tick, the message is
`feat: add the O(1) round-robin subject-detection schedule (200 ms tick, per measured detector cost)`
and the first assertion's expected count in the test becomes 5, not 10.)

---

### Task 6: Temporal smoothing — deadband, rate limit, dropout hold

**Files:**
- Create: `src/subject-smoothing.h`
- Create: `tests/subject-smoothing-test.cpp`
- Modify: `CMakeLists.txt` (test registration)

**Interfaces:**
- Consumes: `SubjectFrame` from Task 4.
- Produces:
  - `struct SubjectSmoothingConfig { float deadband; float max_travel_per_sec; uint64_t dropout_hold_ns; float min_confidence; uint64_t max_step_dt_ns; };`
  - `float subject_smooth_coord(float current, float target, float deadband, float max_step);`
  - `class SubjectSmoother` with `explicit SubjectSmoother(SubjectSmoothingConfig cfg = {})`, `void update(const SubjectFrame &raw, uint64_t now_ns)`, `SubjectFrame output() const`, `void reset()`.

- [ ] **Step 1: Write the failing test**

`tests/subject-smoothing-test.cpp`:

```cpp
// tests/subject-smoothing-test.cpp
// Deadband, rate limit and dropout hold, which the spec calls mandatory rather
// than optional.
//
// Two of these assertions describe a live failure the feature would otherwise
// ship with. A panelist who turns their head for half a second must NOT be
// told "step into frame" -- that is a tool actively lying to the person it is
// supposed to help, and the raw detector produces exactly that, because a
// profile view drops below the confidence bar for a few detections. And a
// motionless panelist must produce ZERO tile movement: the detector's box
// jitters by a pixel or two between frames on a completely still subject, and
// without a deadband that jitter becomes a permanently crawling tile.

#include "subject-smoothing.h"

#include <cstdint>
#include <iostream>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static void check_near(float got, float want, float tol, const char *what)
{
    const float d = got > want ? got - want : want - got;
    if (d > tol) {
        std::cerr << "FAIL: " << what << " got " << got << " want " << want
                  << "\n";
        ++g_failures;
    }
}

static SubjectFrame detection(float x, float y, float conf, uint64_t ns)
{
    SubjectFrame f{};
    f.found = true;
    f.box_x = x;
    f.box_y = y;
    f.box_w = 0.25f;
    f.box_h = 0.40f;
    f.eye_l_x = x + 0.06f;
    f.eye_l_y = y + 0.12f;
    f.eye_r_x = x + 0.18f;
    f.eye_r_y = y + 0.12f;
    f.confidence = conf;
    f.detected_ns = ns;
    return f;
}

static const uint64_t kTick = 100000000ull;  // 100 ms, the schedule's tick

int main()
{
    const SubjectSmoothingConfig cfg{};  // deadband 0.01, 0.35/s, 1.5 s hold

    // ---- the first detection snaps --------------------------------------
    // Ramping in from (0,0) would sweep a tile across the frame on the first
    // acquisition, which is the most visible moment there is.
    {
        SubjectSmoother s(cfg);
        const SubjectFrame raw = detection(0.30f, 0.20f, 0.9f, 500);
        s.update(raw, kTick);
        const SubjectFrame out = s.output();
        check(out.found, "the first detection is found");
        check_near(out.box_x, 0.30f, 0.0001f, "first detection SNAPS box_x");
        check_near(out.box_y, 0.20f, 0.0001f, "first detection SNAPS box_y");
        check_near(out.eye_l_x, 0.36f, 0.0001f, "first detection snaps eye_l_x");
        check(out.detected_ns == 500,
              "detected_ns is the source timestamp, not the smoother's clock");
    }

    // ---- deadband: jitter produces ZERO movement -------------------------
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        const float x0 = s.output().box_x;
        const float y0 = s.output().box_y;

        // 50 detections wobbling +/-0.005 around the seed. Deadband is 0.01,
        // so not one of them may move the output by any amount at all --
        // "small movements" is not good enough, a tile that creeps is a tile
        // an operator notices.
        for (int i = 0; i < 50; ++i) {
            t += kTick;
            const float wobble = (i % 2 == 0) ? 0.005f : -0.004f;
            s.update(detection(0.30f + wobble, 0.20f - wobble, 0.9f, t), t);
        }
        check(s.output().box_x == x0,
              "jitter inside the deadband produces EXACTLY zero movement in x");
        check(s.output().box_y == y0,
              "jitter inside the deadband produces EXACTLY zero movement in y");
    }

    // A real move, larger than the deadband, does eventually get there.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        for (int i = 0; i < 60; ++i) {
            t += kTick;
            s.update(detection(0.60f, 0.20f, 0.9f, t), t);
        }
        check_near(s.output().box_x, 0.60f, 0.02f,
                   "a real move converges on the target, it is not just damped");
    }

    // ---- rate limit ------------------------------------------------------
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.10f, 0.20f, 0.9f, t), t);

        // Jump the target to 0.90 in one 100 ms step. max_travel_per_sec is
        // 0.35, so the step is at most 0.035; with the deadband taken off the
        // wanted delta first, the output lands at 0.135.
        t += kTick;
        s.update(detection(0.90f, 0.20f, 0.9f, t), t);
        check_near(s.output().box_x, 0.135f, 0.002f,
                   "a big jump is rate-limited to max_travel_per_sec * dt");
        check(s.output().box_x < 0.20f,
              "the output must not teleport to the new position");
    }

    // The rate limit is per SECOND, not per update: a longer gap between
    // detections allows proportionally more travel.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.10f, 0.20f, 0.9f, t), t);
        t += 4 * kTick;  // 400 ms
        s.update(detection(0.90f, 0.20f, 0.9f, t), t);
        check_near(s.output().box_x, 0.10f + 0.35f * 0.4f, 0.003f,
                   "400 ms allows 4x the travel of 100 ms");
    }

    // ...but only up to a cap. After a long stall (an idle meeting, a source
    // that stopped sending) the first detection back must not be allowed a
    // travel budget of several seconds, which would be a teleport wearing a
    // rate limit's clothes.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.10f, 0.20f, 0.9f, t), t);
        t += 30000000000ull;  // 30 seconds
        s.update(detection(0.90f, 0.20f, 0.9f, t), t);
        check(s.output().box_x < 0.35f,
              "a long stall does not bank an unbounded travel budget");
    }

    // ---- dropout hold ----------------------------------------------------
    // THE ASSERTION THIS FILE EXISTS FOR: a brief dropout must not flip state
    // to "no subject". A panelist turning their head drops the detector below
    // its confidence bar for a few hundred milliseconds; telling them to "step
    // into frame" for that is the tool lying to the person it is helping.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        const float held_x = s.output().box_x;
        const uint64_t held_ns = s.output().detected_ns;

        // 10 consecutive misses at 100 ms = 1.0 s, inside the 1.5 s hold.
        for (int i = 0; i < 10; ++i) {
            t += kTick;
            s.update(SubjectFrame{}, t);
            check(s.output().found,
                  "a brief dropout must NOT flip state to no-subject");
        }
        check(s.output().box_x == held_x,
              "the held result keeps its last good position, unmoved");
        check(s.output().detected_ns == held_ns,
              "the held result keeps the LAST REAL detection's timestamp, so a "
              "consumer can tell how stale it is");

        // Past the hold, it does give up -- "step into frame" is correct once
        // somebody has genuinely left.
        t += 6 * kTick;  // total 1.6 s of dropout
        s.update(SubjectFrame{}, t);
        check(!s.output().found,
              "past the hold period the subject IS declared gone");
        check(s.output().confidence == 0.0f,
              "a gone subject reports a zeroed frame, not stale coordinates");
    }

    // A detection arriving during the hold resumes smoothly from the HELD
    // position -- not from wherever the raw detector happens to be, and not by
    // re-snapping, which would make every head turn a visible jump.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        for (int i = 0; i < 5; ++i) { t += kTick; s.update(SubjectFrame{}, t); }
        t += kTick;
        s.update(detection(0.80f, 0.20f, 0.9f, t), t);
        check(s.output().found, "the subject is still found after recovery");
        check(s.output().box_x < 0.55f,
              "recovery is rate-limited from the held position, not a snap");
        check(s.output().box_x > 0.30f, "recovery does move toward the target");
    }

    // Re-acquisition AFTER the hold expired snaps, because there is no longer
    // a held position to travel from -- sliding a tile in from a stale
    // location the subject has left is worse than a cut.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        t += 20 * kTick;  // 2.0 s of nothing: well past the hold
        s.update(SubjectFrame{}, t);
        check(!s.output().found, "gone before re-acquisition");
        t += kTick;
        s.update(detection(0.80f, 0.20f, 0.9f, t), t);
        check_near(s.output().box_x, 0.80f, 0.0001f,
                   "re-acquisition after a full dropout SNAPS");
    }

    // ---- confidence gate -------------------------------------------------
    // A low-confidence detection is treated as a MISS, not as data. Acting on
    // a 30%-confidence ghost would move a tile onto a bookshelf.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        const float x0 = s.output().box_x;
        t += kTick;
        s.update(detection(0.90f, 0.60f, 0.20f, t), t);
        check(s.output().found, "a low-confidence frame is held, not dropped");
        check(s.output().box_x == x0,
              "a low-confidence frame must not move the output at all");
    }

    // A low-confidence run must still eventually time out, exactly like an
    // absent one -- otherwise a persistent false positive holds "found"
    // forever.
    {
        SubjectSmoother s(cfg);
        uint64_t t = kTick;
        s.update(detection(0.30f, 0.20f, 0.9f, t), t);
        for (int i = 0; i < 20; ++i) {
            t += kTick;
            s.update(detection(0.30f, 0.20f, 0.10f, t), t);
        }
        check(!s.output().found,
              "a long run of low-confidence frames times out like a dropout");
    }

    // ---- reset -----------------------------------------------------------
    // A participant leaving and rejoining, or a slot repointing, must not
    // inherit the previous person's position.
    {
        SubjectSmoother s(cfg);
        s.update(detection(0.30f, 0.20f, 0.9f, kTick), kTick);
        s.reset();
        check(!s.output().found, "reset clears the held result");
        s.update(detection(0.70f, 0.50f, 0.9f, 2 * kTick), 2 * kTick);
        check_near(s.output().box_x, 0.70f, 0.0001f,
                   "the first detection after a reset snaps, like a first one");
    }

    // ---- the coordinate primitive itself --------------------------------
    {
        check(subject_smooth_coord(0.5f, 0.505f, 0.01f, 1.0f) == 0.5f,
              "inside the deadband: unchanged, bit for bit");
        check(subject_smooth_coord(0.5f, 0.495f, 0.01f, 1.0f) == 0.5f,
              "inside the deadband in the negative direction too");
        check_near(subject_smooth_coord(0.5f, 0.6f, 0.01f, 1.0f), 0.59f, 0.0001f,
                   "outside the deadband: moves by delta MINUS the deadband, so "
                   "crossing the threshold by a hair is not a jump");
        check_near(subject_smooth_coord(0.5f, 0.6f, 0.01f, 0.02f), 0.52f,
                   0.0001f, "the step is capped by max_step");
        check_near(subject_smooth_coord(0.5f, 0.4f, 0.01f, 0.02f), 0.48f,
                   0.0001f, "the cap applies symmetrically downward");
    }

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "subject-smoothing OK\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoSubjectSmoothingTest`
Expected: FAIL — target does not exist; once registered, `Cannot open include file: 'subject-smoothing.h'`.

- [ ] **Step 3: Write the header**

`src/subject-smoothing.h`:

```cpp
#pragma once

// Making the detector's raw output fit to drive a tile crop and a person's
// framing advice.
//
// Extracted so it can be tested without a detector, a thread or a GPU -- the
// same treatment director-handover.h and iso-video-pacer.h get, and for the
// same reason: every failure here is a behaviour on air that no other test in
// this repo would notice.
//
// RAW DETECTION OUTPUT IS NOT USABLE AS-IS, in three separate ways, and the
// spec calls all three mandatory rather than optional:
//
// 1. IT JITTERS. The box moves a pixel or two between detections on a
//    completely motionless subject. Fed straight to a tile crop that is a tile
//    that never stops crawling -- the single most noticeable artefact
//    auto-framing can have. The DEADBAND answers it, and the bar is exact
//    zero movement, not small movement: a tile that creeps slowly is still a
//    tile an operator sees.
//
// 2. IT STEPS. A person leaning across their desk moves the box a long way
//    between two detections 100 ms apart. Followed literally the tile snaps.
//    The RATE LIMIT answers it, in normalized units per second so it is
//    independent of how often detections actually arrive.
//
// 3. IT DROPS OUT. A head turn takes the face below the confidence bar for a
//    few detections. Reported honestly that reads as "no subject", and the
//    return overlay tells a correctly seated panelist to STEP INTO FRAME --
//    the tool actively lying to the person it exists to help. The DROPOUT
//    HOLD answers it: keep the last good result for a while before declaring
//    anyone gone.
//
// WHY A LOW-CONFIDENCE DETECTION IS TREATED AS A MISS, NOT AS DATA: acting on
// a 30%-confidence hit moves a tile onto a bookshelf. It is held exactly like
// an absence, which also means a long run of them times out like one, so a
// persistent false positive cannot hold "found" forever.
//
// WHY THE FIRST DETECTION SNAPS: ramping in from (0,0) sweeps the crop across
// the frame at the most visible moment there is -- acquisition. Same after the
// hold expires: there is no longer a held position worth travelling from, and
// sliding in from where somebody used to be is worse than a cut.
//
// One SubjectSmoother per participant. Not thread-safe; the detector worker
// owns them all and is the only thread that touches them.

#include "subject-frame.h"

#include <cstdint>

struct SubjectSmoothingConfig {
    // Movement below this (normalized units, so a fraction of the frame) is
    // ignored entirely. 0.01 is 1% of the frame -- ~19 px of a 1920 wide
    // source, comfortably above detector jitter and well below a real shift
    // of position.
    float deadband = 0.01f;

    // Maximum travel per second, normalized units. 0.35 crosses a third of the
    // frame in a second: fast enough to follow someone changing seats within a
    // couple of seconds, slow enough that no single detection can snap a tile.
    float max_travel_per_sec = 0.35f;

    // How long a good result is held before the subject is declared gone.
    // 1.5 s covers a head turn, a hand across the face, and a couple of missed
    // round-robin slots; it is short enough that somebody who really walked
    // away is reported within about two seconds.
    uint64_t dropout_hold_ns = 1500000000ull;

    // Below this, a detection is treated as a miss. Distinct from the
    // detector's own kSubjectMinConfidencePct: that one decides what the CNN
    // reports at all, this one decides what we are willing to MOVE for.
    float min_confidence = 0.5f;

    // Caps the dt used for the rate limit. Without it, the first detection
    // after a long stall (an idle meeting, a source that stopped sending)
    // banks seconds of travel budget and teleports -- a rate limit in name
    // only. 500 ms is five schedule ticks: generous for a real gap, useless
    // as a teleport.
    uint64_t max_step_dt_ns = 500000000ull;
};

// Moves `current` toward `target` subject to a deadband and a maximum step.
//
// Note the deadband is SUBTRACTED from the wanted delta rather than just
// gating it. Gating alone means a movement one unit past the threshold jumps
// the entire delta, so the deadband would create the very discontinuity it
// exists to remove; subtracting makes movement continuous across the
// threshold.
inline float subject_smooth_coord(float current, float target, float deadband,
                                  float max_step)
{
    const float delta = target - current;
    const float mag = delta < 0.0f ? -delta : delta;
    if (mag <= deadband) return current;

    float wanted = delta > 0.0f ? (delta - deadband) : (delta + deadband);
    if (wanted > max_step) wanted = max_step;
    if (wanted < -max_step) wanted = -max_step;
    return current + wanted;
}

class SubjectSmoother {
public:
    explicit SubjectSmoother(SubjectSmoothingConfig cfg = {}) : m_cfg(cfg) {}

    // Feed EVERY scheduled result, hit or miss. A miss is a default-
    // constructed SubjectFrame. `now_ns` is a monotonic clock; the smoother
    // measures elapsed time with it, so it must be the same clock every call.
    void update(const SubjectFrame &raw, uint64_t now_ns)
    {
        const bool usable = raw.found && raw.confidence >= m_cfg.min_confidence;

        if (!usable) {
            if (!m_have) return;  // already given up; nothing to hold
            if (now_ns >= m_last_good_ns &&
                now_ns - m_last_good_ns > m_cfg.dropout_hold_ns) {
                // Genuinely gone. Zero the whole frame rather than clearing
                // `found` alone, so a consumer that forgets to check it reads
                // an obviously-empty box instead of a stale position.
                m_have = false;
                m_out = SubjectFrame{};
            }
            // Inside the hold: deliberately change NOTHING. m_out keeps its
            // position, its confidence and the last real detection's
            // detected_ns, which is what lets a consumer age it.
            return;
        }

        if (!m_have) {
            // Acquisition (first ever, or after the hold expired): snap.
            m_out = raw;
            m_have = true;
            m_last_good_ns = now_ns;
            m_last_update_ns = now_ns;
            return;
        }

        uint64_t dt_ns = now_ns > m_last_update_ns ? now_ns - m_last_update_ns : 0;
        if (dt_ns > m_cfg.max_step_dt_ns) dt_ns = m_cfg.max_step_dt_ns;
        const float max_step =
            m_cfg.max_travel_per_sec * (static_cast<float>(dt_ns) / 1e9f);

        m_out.box_x = subject_smooth_coord(m_out.box_x, raw.box_x, m_cfg.deadband, max_step);
        m_out.box_y = subject_smooth_coord(m_out.box_y, raw.box_y, m_cfg.deadband, max_step);
        m_out.box_w = subject_smooth_coord(m_out.box_w, raw.box_w, m_cfg.deadband, max_step);
        m_out.box_h = subject_smooth_coord(m_out.box_h, raw.box_h, m_cfg.deadband, max_step);
        m_out.eye_l_x = subject_smooth_coord(m_out.eye_l_x, raw.eye_l_x, m_cfg.deadband, max_step);
        m_out.eye_l_y = subject_smooth_coord(m_out.eye_l_y, raw.eye_l_y, m_cfg.deadband, max_step);
        m_out.eye_r_x = subject_smooth_coord(m_out.eye_r_x, raw.eye_r_x, m_cfg.deadband, max_step);
        m_out.eye_r_y = subject_smooth_coord(m_out.eye_r_y, raw.eye_r_y, m_cfg.deadband, max_step);

        // Confidence and the timestamp are NOT smoothed. They describe the
        // detection, not a position: a consumer asking "how confident, how
        // recent" wants the real answer, not a rolling average of one.
        m_out.found = true;
        m_out.confidence = raw.confidence;
        m_out.detected_ns = raw.detected_ns;

        m_last_good_ns = now_ns;
        m_last_update_ns = now_ns;
    }

    SubjectFrame output() const { return m_out; }

    // Forget everything. Call when the participant this smoother tracks
    // changes -- a leave and rejoin, or a tile slot repointing -- so the new
    // person does not inherit the previous one's position and travel to it.
    void reset()
    {
        m_out = SubjectFrame{};
        m_have = false;
        m_last_good_ns = 0;
        m_last_update_ns = 0;
    }

private:
    SubjectSmoothingConfig m_cfg;
    SubjectFrame m_out{};
    bool m_have = false;
    uint64_t m_last_good_ns = 0;
    uint64_t m_last_update_ns = 0;
};
```

- [ ] **Step 4: Register the test**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`:

```cmake
    # Deadband, rate limit and dropout hold on the detector's output. Two of
    # its assertions describe behaviour the feature would otherwise ship as a
    # defect: a motionless panelist must produce EXACTLY zero tile movement,
    # and a brief detection dropout must not tell a correctly framed panelist
    # to step into frame. Header-only, so no extra .cpp.
    add_executable(CoreVideoSubjectSmoothingTest
        tests/subject-smoothing-test.cpp
    )
    target_include_directories(CoreVideoSubjectSmoothingTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSubjectSmoothing
             COMMAND CoreVideoSubjectSmoothingTest)
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoSubjectSmoothing --output-on-failure
```

Expected: PASS, `subject-smoothing OK`.

- [ ] **Step 6: Commit**

```bash
git add src/subject-smoothing.h tests/subject-smoothing-test.cpp CMakeLists.txt
git commit -m "feat: add temporal smoothing (deadband, rate limit, dropout hold) for SubjectFrame"
```

---

### Task 7: The single-slot frame inbox

**Files:**
- Create: `src/subject-frame-inbox.h`
- Create: `tests/subject-inbox-test.cpp`
- Modify: `CMakeLists.txt` (test registration)

**Interfaces:**
- Consumes: nothing.
- Produces: `class SubjectFrameInbox` with
  - `void request(uint32_t participant_id)`
  - `uint32_t wanted() const`
  - `bool offer(uint32_t participant_id, const uint8_t *i420, size_t len, uint32_t width, uint32_t height, uint64_t source_ns)`
  - `bool take(std::vector<uint8_t> &pixels, uint32_t &participant_id, uint32_t &width, uint32_t &height, uint64_t &source_ns)`
  - `void cancel()`
  - `uint64_t offers_accepted() const`, `uint64_t offers_declined() const`

- [ ] **Step 1: Write the failing test**

`tests/subject-inbox-test.cpp`:

```cpp
// tests/subject-inbox-test.cpp
// The handshake that keeps the detector off the engine-IPC reader thread.
//
// WHY THE DECLINE PATH IS THE IMPORTANT ONE. offer() is called from
// tile_feed_on_frame(), on the shared reader thread that dispatches frames for
// EVERY source in the plugin. At a nine-tile wall that is hundreds of calls a
// second, and at most ten of them per second may do any work. So the assertion
// that matters is not "a requested frame arrives" but "an unrequested frame
// costs one atomic load and nothing else" -- no copy, no allocation, and
// above all no lock, because a lock there is the 2026-08-17 head-of-line
// stall (src/media-event-queue.h) rebuilt by hand.

#include "subject-frame-inbox.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static std::vector<uint8_t> fake_i420(uint32_t w, uint32_t h, uint8_t fill)
{
    const size_t y_len = static_cast<size_t>(w) * h;
    return std::vector<uint8_t>(y_len + y_len / 2, fill);
}

int main()
{
    const std::vector<uint8_t> frame_a = fake_i420(64, 64, 0x11);
    const std::vector<uint8_t> frame_b = fake_i420(64, 64, 0x22);

    // ---- nothing requested ----------------------------------------------
    {
        SubjectFrameInbox inbox;
        check(inbox.wanted() == 0, "a fresh inbox wants nothing");
        check(!inbox.offer(7, frame_a.data(), frame_a.size(), 64, 64, 1),
              "an offer with nothing requested is declined");
        check(inbox.offers_declined() == 1, "the decline is counted");
        check(inbox.offers_accepted() == 0, "nothing was accepted");

        std::vector<uint8_t> px;
        uint32_t id = 0, w = 0, h = 0;
        uint64_t ns = 0;
        check(!inbox.take(px, id, w, h, ns), "take on an empty inbox is false");
    }

    // ---- the ordinary round trip ----------------------------------------
    {
        SubjectFrameInbox inbox;
        inbox.request(7);
        check(inbox.wanted() == 7, "the request is visible to the producer");

        // Everyone else is declined -- this is the case that runs hundreds of
        // times a second.
        check(!inbox.offer(8, frame_b.data(), frame_b.size(), 64, 64, 2),
              "a frame from a participant we did not ask for is declined");
        check(!inbox.offer(0, frame_b.data(), frame_b.size(), 64, 64, 2),
              "participant id 0 is never accepted");

        check(inbox.offer(7, frame_a.data(), frame_a.size(), 64, 64, 555),
              "the requested participant's frame is accepted");
        check(inbox.wanted() == 0,
              "an accepted offer clears the request, so the next frame from "
              "the same participant is declined too");
        check(!inbox.offer(7, frame_b.data(), frame_b.size(), 64, 64, 556),
              "a second frame for a satisfied request is declined");

        std::vector<uint8_t> px;
        uint32_t id = 0, w = 0, h = 0;
        uint64_t ns = 0;
        check(inbox.take(px, id, w, h, ns), "the worker takes the frame");
        check(id == 7, "the participant id comes back with the pixels");
        check(w == 64 && h == 64, "the dimensions come back with the pixels");
        check(ns == 555, "the source timestamp comes back with the pixels");
        check(px.size() == frame_a.size(), "the whole frame was copied");
        check(!px.empty() && px[0] == 0x11 && px.back() == 0x11,
              "the pixels are the FIRST offered frame, not a later one");

        check(!inbox.take(px, id, w, h, ns),
              "taking twice yields nothing the second time");
    }

    // ---- cancellation ----------------------------------------------------
    // The worker cancels a request when the participant never sends a frame,
    // so a camera-off panelist cannot wedge the schedule forever.
    {
        SubjectFrameInbox inbox;
        inbox.request(7);
        inbox.cancel();
        check(inbox.wanted() == 0, "cancel clears the request");
        check(!inbox.offer(7, frame_a.data(), frame_a.size(), 64, 64, 1),
              "a cancelled request declines the frame it was waiting for");
    }

    // Re-requesting must discard whatever an earlier request left behind, or
    // the worker could detect participant A's pixels and record the result
    // against participant B.
    {
        SubjectFrameInbox inbox;
        inbox.request(7);
        check(inbox.offer(7, frame_a.data(), frame_a.size(), 64, 64, 1),
              "first request is satisfied");
        inbox.request(9);  // worker moved on without taking
        std::vector<uint8_t> px;
        uint32_t id = 0, w = 0, h = 0;
        uint64_t ns = 0;
        check(!inbox.take(px, id, w, h, ns),
              "a new request discards an untaken frame from the old one");
    }

    // ---- refusals --------------------------------------------------------
    {
        SubjectFrameInbox inbox;
        inbox.request(7);
        check(!inbox.offer(7, nullptr, 0, 64, 64, 1), "a null frame is refused");
        check(inbox.wanted() == 7,
              "a refused offer leaves the request standing, so the next real "
              "frame still satisfies it");
        check(!inbox.offer(7, frame_a.data(), 10, 64, 64, 1),
              "a buffer shorter than w*h*3/2 is refused rather than copied");
        check(!inbox.offer(7, frame_a.data(), frame_a.size(), 0, 64, 1),
              "a zero dimension is refused");
        check(inbox.wanted() == 7, "still standing after every refusal");
    }

    // ---- concurrency -----------------------------------------------------
    // The real shape: one producer hammering offer() the way the IPC reader
    // thread does, one consumer requesting and taking the way the worker does.
    // Under TSan or a debug CRT this is where a torn buffer or a missing lock
    // shows up; here it asserts that the accounting stays exact and nothing
    // deadlocks.
    {
        SubjectFrameInbox inbox;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> accepted{0};

        std::thread producer([&] {
            const std::vector<uint8_t> f = fake_i420(64, 64, 0x33);
            uint64_t ts = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (uint32_t id = 1; id <= 20; ++id) {
                    if (inbox.offer(id, f.data(), f.size(), 64, 64, ++ts))
                        accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        int taken = 0;
        for (int round = 0; round < 200; ++round) {
            const uint32_t want = static_cast<uint32_t>((round % 20) + 1);
            inbox.request(want);
            std::vector<uint8_t> px;
            uint32_t id = 0, w = 0, h = 0;
            uint64_t ns = 0;
            for (int spin = 0; spin < 100000; ++spin) {
                if (inbox.take(px, id, w, h, ns)) {
                    ++taken;
                    check(id == want,
                          "a taken frame is always the one that was requested");
                    check(px.size() == static_cast<size_t>(64) * 64 * 3 / 2,
                          "a taken frame is never torn or short");
                    break;
                }
                std::this_thread::yield();
            }
            inbox.cancel();
        }

        stop.store(true, std::memory_order_relaxed);
        producer.join();

        check(taken > 100,
              "the worker gets its frames under a hammering producer");
        check(inbox.offers_accepted() == accepted.load(),
              "accepted offers are counted exactly once");
        std::cerr << "concurrent round trip: " << taken << " of 200 taken, "
                  << inbox.offers_declined() << " offers declined\n";
    }

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "subject-frame-inbox OK\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoSubjectInboxTest`
Expected: FAIL — target does not exist; once registered, `Cannot open include file: 'subject-frame-inbox.h'`.

- [ ] **Step 3: Write the header**

`src/subject-frame-inbox.h`:

```cpp
#pragma once

// The one-slot handoff between the engine-IPC reader thread and the subject
// detector's worker thread.
//
// Extracted so the whole handshake can be tested with two plain std::threads
// and no engine, the same treatment media-event-queue.h gets, and for a
// related reason: getting this wrong does not break the detector, it breaks
// EVERY source in the plugin.
//
// THE CONSTRAINT THIS FILE EXISTS FOR. The frame callback
// (tile_feed_on_frame, src/zoom-supersource.cpp) runs on the shared engine-IPC
// reader thread that dispatches frames for every source in the plugin.
// Anything slow there head-of-line-blocks all of them -- that is the
// 2026-08-17 incident recorded on src/media-event-queue.h, where inline media
// work on that thread starved audio events by up to a second and cost ~92% of
// the audio on every source. So detection cannot run there, and neither can
// anything that waits.
//
// WHY ONE SLOT AND NOT A MAP. The schedule (src/subject-schedule.h) issues
// exactly one detection at a time, so at most one frame can ever be in flight.
// A map keyed by participant would need a mutex on the producer's fast path to
// find the entry -- a shared lock taken hundreds of times a second by the
// thread that must never wait. One slot needs no lookup at all: the producer's
// fast path is a single atomic load that fails for every participant except
// the one currently wanted, which is the overwhelming majority of calls.
//
// WHY THE PRODUCER STILL TAKES A MUTEX ON THE ACCEPTED PATH. Copying the frame
// is not atomic, and the worker must not read a half-written buffer. That lock
// is taken at most once per schedule tick (ten times a second), it is
// uncontended in practice because the worker holds it only for a swap, and the
// copy under it is one 360p frame -- ~150 KB, tens of microseconds. That is
// the entire cost this design imposes on the shared reader thread, and it is
// bounded by the TICK, not by the frame rate or the number of sources.
//
// The claim is done with a compare-exchange rather than a plain store so that
// two producers (a second video source offering the same participant) cannot
// both decide they won and both copy.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class SubjectFrameInbox {
public:
    // Worker side: ask for the next frame from `participant_id`. Discards any
    // frame an earlier request left untaken -- keeping it would let the worker
    // detect one participant's pixels and record the result against another.
    void request(uint32_t participant_id)
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_ready = false;
        }
        m_wanted.store(participant_id, std::memory_order_release);
    }

    // Worker side: stop waiting. Used when a requested participant does not
    // send a frame within a few ticks -- a camera-off panelist must not wedge
    // the schedule.
    void cancel() { m_wanted.store(0, std::memory_order_release); }

    uint32_t wanted() const { return m_wanted.load(std::memory_order_acquire); }

    // Producer side, on the engine-IPC reader thread.
    //
    // The fast path -- the one that runs for every frame of every participant
    // we are not currently waiting on -- is one atomic load and a return. No
    // lock, no copy, no allocation.
    //
    // Returns true only when the pixels were copied. A refusal (null, short
    // buffer, nonsense dimensions) leaves the request STANDING, so the next
    // good frame from that participant still satisfies it.
    bool offer(uint32_t participant_id, const uint8_t *i420, size_t len,
               uint32_t width, uint32_t height, uint64_t source_ns)
    {
        if (participant_id == 0) {
            m_declined.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (m_wanted.load(std::memory_order_acquire) != participant_id) {
            m_declined.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!i420 || width < 2 || height < 2) return false;
        const size_t need = static_cast<size_t>(width) * height * 3 / 2;
        if (len < need) return false;

        // Claim before copying: two producers offering the same participant
        // must not both copy into the slot.
        uint32_t expected = participant_id;
        if (!m_wanted.compare_exchange_strong(expected, 0u,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            m_declined.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_pixels.assign(i420, i420 + need);
            m_participant_id = participant_id;
            m_width = width;
            m_height = height;
            m_source_ns = source_ns;
            m_ready = true;
        }
        m_accepted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Worker side: take the delivered frame, if there is one. Swaps rather
    // than copies, handing the caller's previous buffer back to be refilled,
    // so neither side allocates after warm-up -- the same trick
    // tile_take_snapshot uses.
    bool take(std::vector<uint8_t> &pixels, uint32_t &participant_id,
              uint32_t &width, uint32_t &height, uint64_t &source_ns)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_ready) return false;
        pixels.swap(m_pixels);
        participant_id = m_participant_id;
        width = m_width;
        height = m_height;
        source_ns = m_source_ns;
        m_ready = false;
        return true;
    }

    uint64_t offers_accepted() const
    {
        return m_accepted.load(std::memory_order_relaxed);
    }
    uint64_t offers_declined() const
    {
        return m_declined.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> m_wanted{0};
    std::atomic<uint64_t> m_accepted{0};
    std::atomic<uint64_t> m_declined{0};

    mutable std::mutex m_mtx;
    std::vector<uint8_t> m_pixels;
    uint32_t m_participant_id = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_source_ns = 0;
    bool m_ready = false;
};
```

- [ ] **Step 4: Register the test**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`:

```cmake
    # The one-slot handoff from the engine-IPC reader thread to the detector
    # worker. Exercised with concurrent threads, like CoreVideoTileSlot: the
    # assertion that matters is that an UNREQUESTED frame costs one atomic load
    # and nothing else, because that path runs hundreds of times a second on
    # the thread that serves every source in the plugin.
    add_executable(CoreVideoSubjectInboxTest
        tests/subject-inbox-test.cpp
    )
    target_include_directories(CoreVideoSubjectInboxTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(CoreVideoSubjectInboxTest PRIVATE Threads::Threads)
    add_test(NAME CoreVideoSubjectInbox
             COMMAND CoreVideoSubjectInboxTest)
```

(If `find_package(Threads REQUIRED)` is already called earlier in the file — check with `grep -n "find_package(Threads" CMakeLists.txt` — drop that line and keep only the `target_link_libraries`.)

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoSubjectInbox --output-on-failure
```

Expected: PASS, with a line like `concurrent round trip: 200 of 200 taken, N offers declined` and `subject-frame-inbox OK`.

- [ ] **Step 6: Commit**

```bash
git add src/subject-frame-inbox.h tests/subject-inbox-test.cpp CMakeLists.txt
git commit -m "feat: add the single-slot frame inbox that keeps detection off the IPC reader thread"
```

---

### Task 8: The detector worker thread, the public API, and the plugin tap

**Files:**
- Create: `src/subject-detector-engine.h`
- Create: `src/subject-detector-engine.cpp`
- Create: `tests/subject-worker-test.cpp`
- Modify: `CMakeLists.txt` (plugin source, test registration)
- Modify: `src/zoom-supersource.cpp` (the frame tap in `tile_feed_on_frame`, and roster/speaker pushes)
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: `SubjectFrame` and `ISubjectDetector` / `make_facedetect_cnn_detector` (Task 4), `i420_to_bgr_downscale` / `kSubjectLongEdge` / `BgrImage` (Task 3), `SubjectSchedule` / `SubjectScheduleConfig` (Task 5), `SubjectSmoother` / `SubjectSmoothingConfig` (Task 6), `SubjectFrameInbox` (Task 7).
- Produces — **this is the API Subsystems 3a and 3b consume, and the end of this plan's scope:**

```cpp
class SubjectDetectorEngine {
public:
    static SubjectDetectorEngine &instance();
    void start();
    void stop();
    bool running() const;
    void set_monitored(const std::vector<uint32_t> &participant_ids);
    void set_speaker_poll(std::function<uint32_t()> poll);
    bool offer_frame(uint32_t participant_id, const uint8_t *i420, size_t len,
                     uint32_t width, uint32_t height, uint64_t source_ns);
    SubjectFrame subject(uint32_t participant_id) const;
    uint64_t detections_completed() const;
    uint64_t requests_timed_out() const;
    // Test seams; both must be called before start().
    void set_detector_factory(std::function<std::unique_ptr<ISubjectDetector>()> factory);
    void set_schedule_config(SubjectScheduleConfig cfg);
};
```

- [ ] **Step 1: Write the failing test**

`tests/subject-worker-test.cpp`:

```cpp
// tests/subject-worker-test.cpp
// The detector worker: one thread, driven by the schedule, fed by the inbox,
// with results landing in the per-participant smoothers.
//
// It runs against a FAKE ISubjectDetector, which is the entire reason the
// narrow interface exists. That buys three things no test with the real CNN
// could have: a deterministic result (so the assertions are about the worker,
// not about whether YuNet found a synthetic face), a millisecond-scale test
// (the real detector would need seconds of wall time to produce enough ticks),
// and -- the important one -- a THREAD-IDENTITY assertion. The fake records
// which thread called it, so the test can prove detection never runs on the
// thread that offered the frame. That is the spec's hardest constraint and
// nothing else in the suite would notice it being violated: the plugin would
// simply get slower for every source at once, which is the 2026-08-17
// signature.

#include "subject-detector-engine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

// Shared observations, because the engine owns the detector object itself.
static std::atomic<uint64_t> g_detect_calls{0};
static std::atomic<bool> g_saw_foreign_thread{false};
static std::thread::id g_detector_thread{};
static std::atomic<bool> g_detector_thread_set{false};
static std::atomic<int> g_last_width{0};
static std::atomic<int> g_last_height{0};
static std::atomic<unsigned> g_last_centre_b{0};

class FakeDetector : public ISubjectDetector {
public:
    SubjectFrame detect(const uint8_t *bgr, int width, int height,
                        uint64_t source_ns) override
    {
        // Every call must come from the SAME thread, and it must not be a
        // thread the test offered a frame on.
        if (!g_detector_thread_set.exchange(true)) {
            g_detector_thread = std::this_thread::get_id();
        } else if (std::this_thread::get_id() != g_detector_thread) {
            g_saw_foreign_thread.store(true);
        }
        g_detect_calls.fetch_add(1, std::memory_order_relaxed);

        g_last_width.store(width);
        g_last_height.store(height);
        if (bgr && width > 0 && height > 0) {
            const size_t centre =
                (static_cast<size_t>(height / 2) * width + width / 2) * 3;
            g_last_centre_b.store(bgr[centre]);
        }

        SubjectFrame f{};
        f.found = true;
        f.box_x = 0.25f;
        f.box_y = 0.20f;
        f.box_w = 0.30f;
        f.box_h = 0.45f;
        f.eye_l_x = 0.32f;
        f.eye_l_y = 0.34f;
        f.eye_r_x = 0.46f;
        f.eye_r_y = 0.34f;
        f.confidence = 0.95f;
        f.detected_ns = source_ns;
        return f;
    }
};

// A flat mid-grey I420 frame. Y=128, U=V=128 converts to BGR (128,128,128),
// which is what the centre-pixel assertion below checks -- proving the worker
// really ran the conversion rather than handing raw I420 to the detector.
static std::vector<uint8_t> grey_i420(uint32_t w, uint32_t h)
{
    const size_t y_len = static_cast<size_t>(w) * h;
    return std::vector<uint8_t>(y_len + y_len / 2, 128);
}

int main()
{
    SubjectDetectorEngine &engine = SubjectDetectorEngine::instance();

    engine.set_detector_factory(
        [] { return std::unique_ptr<ISubjectDetector>(new FakeDetector()); });

    // A fast tick so the test finishes in milliseconds instead of seconds.
    SubjectScheduleConfig cfg;
    cfg.tick_ns = 2000000ull;  // 2 ms
    cfg.speaker_every = 3;
    engine.set_schedule_config(cfg);

    engine.set_monitored({101, 102, 103});
    engine.set_speaker_poll([] { return 101u; });
    engine.start();
    check(engine.running(), "the engine reports running after start()");

    // ---- feed it, from THIS thread --------------------------------------
    const std::vector<uint8_t> frame = grey_i420(640, 360);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    uint64_t offered = 0;
    while (engine.detections_completed() < 30 &&
           std::chrono::steady_clock::now() < deadline) {
        for (uint32_t id = 101; id <= 103; ++id) {
            if (engine.offer_frame(id, frame.data(), frame.size(), 640, 360,
                                   1000 + offered))
                ++offered;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    check(engine.detections_completed() >= 30,
          "the worker completes detections when frames are offered");

    // ---- the constraint the spec calls non-negotiable --------------------
    check(!g_saw_foreign_thread.load(),
          "every detection ran on ONE thread");
    check(g_detector_thread_set.load() &&
              g_detector_thread != std::this_thread::get_id(),
          "detection NEVER runs on the thread that offered the frame -- that "
          "thread is the shared engine-IPC reader in production and blocking "
          "it stalls every source in the plugin");

    // ---- the conversion really happened ---------------------------------
    check(g_last_width.load() == 320 && g_last_height.load() == 180,
          "the detector is handed the DOWNSCALED image (320x180), not the "
          "640x360 source");
    check(g_last_centre_b.load() >= 126 && g_last_centre_b.load() <= 130,
          "the detector is handed BGR (mid-grey converts to ~128), not raw "
          "I420 planes");

    // ---- results reach the consumers ------------------------------------
    {
        const SubjectFrame f = engine.subject(101);
        check(f.found, "the active speaker has a subject");
        check(f.box_x > 0.2f && f.box_x < 0.3f,
              "the smoothed box matches what the detector returned");
        check(f.confidence > 0.9f, "confidence is carried through");

        const SubjectFrame other = engine.subject(102);
        check(other.found,
              "a non-speaking monitored participant also gets detected");

        const SubjectFrame none = engine.subject(999);
        check(!none.found,
              "an unmonitored participant reports a clean not-found rather "
              "than anything stale or invented");
    }

    // ---- an unmonitored participant's frames are refused -----------------
    {
        const uint64_t before = engine.detections_completed();
        for (int i = 0; i < 50; ++i)
            engine.offer_frame(777, frame.data(), frame.size(), 640, 360, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(engine.subject(777).found == false,
              "offering frames for someone not monitored never produces a "
              "subject for them");
        (void)before;
    }

    // ---- a silent participant must not wedge the schedule ---------------
    // 104 is monitored but never sends a frame. The worker must time its
    // request out and carry on, or one camera-off panelist stops detection for
    // everybody.
    {
        engine.set_monitored({101, 104});
        const uint64_t timeouts_before = engine.requests_timed_out();
        const uint64_t detections_before = engine.detections_completed();
        const auto t_end = std::chrono::steady_clock::now() +
                           std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < t_end &&
               engine.detections_completed() < detections_before + 20) {
            engine.offer_frame(101, frame.data(), frame.size(), 640, 360, 9);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(engine.detections_completed() >= detections_before + 20,
              "a participant who never sends a frame does not stop detection "
              "for the ones who do");
        check(engine.requests_timed_out() > timeouts_before,
              "the silent participant's requests are timed out, not left "
              "outstanding forever");
        check(!engine.subject(104).found,
              "a participant who never sends a frame has no subject");
    }

    // ---- removing a participant clears their state -----------------------
    {
        check(engine.subject(101).found, "101 has a subject before removal");
        engine.set_monitored({102});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!engine.subject(101).found,
              "a participant dropped from the monitored set stops reporting a "
              "subject -- a rejoining person must not inherit the old one's "
              "framing");
    }

    // ---- shutdown --------------------------------------------------------
    engine.stop();
    check(!engine.running(), "the engine reports stopped after stop()");
    {
        const uint64_t after_stop = engine.detections_completed();
        for (int i = 0; i < 20; ++i)
            engine.offer_frame(102, frame.data(), frame.size(), 640, 360, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(engine.detections_completed() == after_stop,
              "no detection happens after stop()");
    }
    engine.stop();  // idempotent: a second stop must not hang or crash
    engine.start();
    engine.stop();  // restartable

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cerr << "subject-worker OK (" << g_detect_calls.load()
              << " detections)\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target CoreVideoSubjectWorkerTest`
Expected: FAIL — target does not exist; once registered, `Cannot open include file: 'subject-detector-engine.h'`.

- [ ] **Step 3: Write the engine header**

`src/subject-detector-engine.h`:

```cpp
#pragma once

// The subject detector as a running thing: one worker thread, the schedule
// that decides who it looks at, the inbox that feeds it, and one smoother per
// participant.
//
// THIS HEADER IS THE END OF SUBSYSTEM 2 AND THE START OF SUBSYSTEMS 3a AND 3b.
// Tiles auto-framing and the return-feed overlay both consume `subject()` and
// nothing else. Everything below this line -- the CNN, the colour convert, the
// round robin, the smoothing -- is private to that promise.
//
// THREADING CONTRACT, which is the whole point of the class:
//
//   offer_frame()  is called ON THE ENGINE-IPC READER THREAD, the shared
//                  thread that dispatches frames for every source in the
//                  plugin. It is cheap by construction: one atomic load for
//                  every participant we are not currently waiting on, and at
//                  most one ~150 KB copy per schedule tick for the one we are.
//                  It NEVER runs a detection, never allocates on the declined
//                  path, and never waits on the worker.
//
//   detect()       runs ONLY on this class's own worker thread.
//
//   subject()      is safe from any thread (a mutex and a map lookup) and is
//                  expected to be called from the OBS graphics thread once per
//                  frame per tile.
//
//   set_monitored() / set_speaker_poll() are safe from any thread.
//
// WHY A SINGLETON. It mirrors ZoomEngineClient: there is one Zoom session, one
// engine process, and one detector budget for the whole plugin. Two instances
// would each run a worker and each pay the schedule's cost, which is exactly
// the O(n) growth the schedule exists to prevent.

#include "subject-frame.h"
#include "subject-detector.h"
#include "subject-schedule.h"
#include "subject-smoothing.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class SubjectDetectorEngine {
public:
    static SubjectDetectorEngine &instance();

    SubjectDetectorEngine(const SubjectDetectorEngine &) = delete;
    SubjectDetectorEngine &operator=(const SubjectDetectorEngine &) = delete;

    // Idempotent. Spawns the single worker thread and builds the detector from
    // the current factory.
    void start();
    // Idempotent, and safe to call from a different thread than start().
    // Joins the worker before returning.
    void stop();
    bool running() const;

    // The participants worth detecting -- in practice the ones actually being
    // SHOWN, not the whole meeting. Participants dropped from this set have
    // their smoother state discarded, so a rejoining person never inherits the
    // previous occupant's framing.
    void set_monitored(const std::vector<uint32_t> &participant_ids);

    // How the worker learns who is on air. SpeakerDirector is POLL-ONLY --
    // there is no observer list -- so the worker polls this once per tick.
    // The plugin passes a closure over
    // ZoomEngineClient::instance().active_speaker_id(): the DIRECTED id, after
    // hold and dwell, never the raw one.
    //
    // Injected rather than called directly so this whole class stays free of
    // libobs, Qt and the engine client, and can therefore be tested off-rig.
    void set_speaker_poll(std::function<uint32_t()> poll);

    // Producer side. Returns true only when the frame was taken. `i420` is
    // tightly packed I420 (Y, then U, then V), BT.709 full range.
    bool offer_frame(uint32_t participant_id, const uint8_t *i420, size_t len,
                     uint32_t width, uint32_t height, uint64_t source_ns);

    // Consumer side. The smoothed result, or a default (not-found) SubjectFrame
    // for anyone unknown.
    SubjectFrame subject(uint32_t participant_id) const;

    uint64_t detections_completed() const;
    uint64_t requests_timed_out() const;

    // Test seams. Both must be called before start(); calling them on a
    // running engine is ignored.
    void set_detector_factory(
        std::function<std::unique_ptr<ISubjectDetector>()> factory);
    void set_schedule_config(SubjectScheduleConfig cfg);

private:
    SubjectDetectorEngine();
    ~SubjectDetectorEngine();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
```

- [ ] **Step 4: Write the engine implementation**

`src/subject-detector-engine.cpp`:

```cpp
#include "subject-detector-engine.h"

#include "i420-bgr-downscale.h"
#include "subject-detector-fd-record.h"
#include "subject-frame-inbox.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

// The worker's own clock. steady_clock rather than os_gettime_ns() so this
// file needs no libobs -- which is what lets tests/subject-worker-test.cpp
// link it without OBS headers. Only elapsed time matters here; nothing
// compares this against a media timestamp.
uint64_t worker_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// How long a request waits for a frame before it is abandoned. Three ticks:
// long enough that an ordinary 30 fps source always answers, short enough that
// a camera-off participant costs the schedule three slots and not the show.
constexpr int kRequestTimeoutTicks = 3;

// The worker's poll interval. Well under the schedule tick, so the tick is the
// thing that paces detection and this is only how promptly a delivered frame
// is picked up.
constexpr int kWorkerPollMs = 2;

}  // namespace

struct SubjectDetectorEngine::Impl {
    // --- configuration, read by the worker at start() ---
    std::function<std::unique_ptr<ISubjectDetector>()> factory;
    SubjectScheduleConfig sched_cfg{};
    SubjectSmoothingConfig smooth_cfg{};

    // --- the producer/consumer seam ---
    SubjectFrameInbox inbox;

    // --- worker lifetime ---
    std::mutex life_mtx;             // serializes start()/stop()
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_flag{false};

    // --- inputs the worker reads, written from other threads ---
    std::mutex input_mtx;
    std::vector<uint32_t> monitored;
    bool monitored_dirty = false;
    std::function<uint32_t()> speaker_poll;

    // --- outputs ---
    mutable std::mutex out_mtx;
    std::unordered_map<uint32_t, SubjectFrame> out;

    std::atomic<uint64_t> detections{0};
    std::atomic<uint64_t> timeouts{0};

    void run();
};

SubjectDetectorEngine::SubjectDetectorEngine() : m_impl(new Impl()) {}

SubjectDetectorEngine::~SubjectDetectorEngine()
{
    stop();
}

SubjectDetectorEngine &SubjectDetectorEngine::instance()
{
    static SubjectDetectorEngine s_instance;
    return s_instance;
}

void SubjectDetectorEngine::set_detector_factory(
    std::function<std::unique_ptr<ISubjectDetector>()> factory)
{
    std::lock_guard<std::mutex> lk(m_impl->life_mtx);
    if (m_impl->running.load()) return;
    m_impl->factory = std::move(factory);
}

void SubjectDetectorEngine::set_schedule_config(SubjectScheduleConfig cfg)
{
    std::lock_guard<std::mutex> lk(m_impl->life_mtx);
    if (m_impl->running.load()) return;
    m_impl->sched_cfg = cfg;
}

void SubjectDetectorEngine::set_monitored(
    const std::vector<uint32_t> &participant_ids)
{
    std::lock_guard<std::mutex> lk(m_impl->input_mtx);
    m_impl->monitored = participant_ids;
    m_impl->monitored_dirty = true;
}

void SubjectDetectorEngine::set_speaker_poll(std::function<uint32_t()> poll)
{
    std::lock_guard<std::mutex> lk(m_impl->input_mtx);
    m_impl->speaker_poll = std::move(poll);
}

void SubjectDetectorEngine::start()
{
    std::lock_guard<std::mutex> lk(m_impl->life_mtx);
    if (m_impl->running.load()) return;
    m_impl->stop_flag.store(false);
    m_impl->running.store(true);
    Impl *impl = m_impl.get();
    m_impl->worker = std::thread([impl] { impl->run(); });
}

void SubjectDetectorEngine::stop()
{
    std::lock_guard<std::mutex> lk(m_impl->life_mtx);
    if (!m_impl->running.load()) return;
    m_impl->stop_flag.store(true);
    m_impl->running.store(false);
    if (m_impl->worker.joinable()) m_impl->worker.join();
    // Nothing is in flight once the worker is joined, so drop the outstanding
    // request rather than leaving a wanted id that a still-running producer
    // would keep copying frames for.
    m_impl->inbox.cancel();
}

bool SubjectDetectorEngine::running() const
{
    return m_impl->running.load();
}

bool SubjectDetectorEngine::offer_frame(uint32_t participant_id,
                                        const uint8_t *i420, size_t len,
                                        uint32_t width, uint32_t height,
                                        uint64_t source_ns)
{
    // No running check here on purpose: stop() cancels the inbox, so with no
    // worker there is never a wanted id and offer() declines on its one atomic
    // load. Adding a second atomic read would only make the hot path -- the
    // one on the shared engine-IPC reader thread -- more expensive.
    return m_impl->inbox.offer(participant_id, i420, len, width, height,
                               source_ns);
}

SubjectFrame SubjectDetectorEngine::subject(uint32_t participant_id) const
{
    std::lock_guard<std::mutex> lk(m_impl->out_mtx);
    auto it = m_impl->out.find(participant_id);
    if (it == m_impl->out.end()) return SubjectFrame{};
    return it->second;
}

uint64_t SubjectDetectorEngine::detections_completed() const
{
    return m_impl->detections.load(std::memory_order_relaxed);
}

uint64_t SubjectDetectorEngine::requests_timed_out() const
{
    return m_impl->timeouts.load(std::memory_order_relaxed);
}

void SubjectDetectorEngine::Impl::run()
{
    // Everything below is worker-thread-local. The schedule, the smoothers and
    // the reusable buffers never leave this function's frame, which is why
    // none of them needs a lock.
    //
    // `factory` is read without a lock deliberately: set_detector_factory()
    // refuses while running, and start() creating this thread is the
    // happens-before edge that publishes it.
    std::unique_ptr<ISubjectDetector> detector =
        factory ? factory()
                : make_facedetect_cnn_detector(kSubjectMinConfidencePct);

    SubjectSchedule schedule(sched_cfg);
    std::unordered_map<uint32_t, SubjectSmoother> smoothers;
    std::unordered_set<uint32_t> monitored_now;

    BgrImage bgr;                 // reused: allocates once per resolution
    std::vector<uint8_t> pixels;  // reused: swapped with the inbox's buffer

    uint32_t outstanding = 0;
    uint64_t outstanding_since = 0;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        // --- pick up roster and speaker changes ---
        uint32_t speaker = 0;
        {
            std::lock_guard<std::mutex> lk(input_mtx);
            if (monitored_dirty) {
                monitored_dirty = false;
                schedule.set_roster(monitored);
                monitored_now.clear();
                for (uint32_t id : monitored) monitored_now.insert(id);

                // Drop everyone who left. A rejoining participant must not
                // inherit the previous occupant's framing, and a stale entry
                // would otherwise answer subject() forever.
                for (auto it = smoothers.begin(); it != smoothers.end();) {
                    if (monitored_now.count(it->first) == 0)
                        it = smoothers.erase(it);
                    else
                        ++it;
                }
                std::lock_guard<std::mutex> ok(out_mtx);
                for (auto it = out.begin(); it != out.end();) {
                    if (monitored_now.count(it->first) == 0)
                        it = out.erase(it);
                    else
                        ++it;
                }
                if (outstanding != 0 && monitored_now.count(outstanding) == 0) {
                    inbox.cancel();
                    outstanding = 0;
                }
            }
            if (speaker_poll) speaker = speaker_poll();
        }
        schedule.set_active_speaker(speaker);

        const uint64_t now = worker_now_ns();

        // --- a delivered frame, if any ---
        uint32_t id = 0, w = 0, h = 0;
        uint64_t source_ns = 0;
        if (inbox.take(pixels, id, w, h, source_ns)) {
            outstanding = 0;
            if (monitored_now.count(id) != 0 &&
                i420_to_bgr_downscale(pixels.data(), pixels.size(),
                                      static_cast<int>(w), static_cast<int>(h),
                                      kSubjectLongEdge, bgr)) {
                const SubjectFrame raw = detector->detect(
                    bgr.pixels.data(), bgr.width, bgr.height, source_ns);

                auto it = smoothers.find(id);
                if (it == smoothers.end())
                    it = smoothers.emplace(id, SubjectSmoother(smooth_cfg)).first;
                it->second.update(raw, worker_now_ns());

                {
                    std::lock_guard<std::mutex> ok(out_mtx);
                    out[id] = it->second.output();
                }
                detections.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (outstanding != 0) {
            // --- time a silent participant's request out ---
            // Without this, one camera-off panelist stops detection for
            // everybody: the schedule would keep issuing while nothing ever
            // satisfied the standing request.
            const uint64_t limit =
                sched_cfg.tick_ns * static_cast<uint64_t>(kRequestTimeoutTicks);
            if (now > outstanding_since && now - outstanding_since > limit) {
                // Feed the smoother a MISS rather than just dropping it, so the
                // dropout-hold clock actually runs for someone who has stopped
                // sending video. Otherwise a participant who switches their
                // camera off would keep reporting their last framing forever.
                auto it = smoothers.find(outstanding);
                if (it != smoothers.end()) {
                    it->second.update(SubjectFrame{}, now);
                    std::lock_guard<std::mutex> ok(out_mtx);
                    out[outstanding] = it->second.output();
                }
                inbox.cancel();
                outstanding = 0;
                timeouts.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // --- issue the next request, if the schedule says it is time ---
        if (outstanding == 0) {
            const uint32_t next = schedule.next(now);
            if (next != 0) {
                inbox.request(next);
                outstanding = next;
                outstanding_since = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kWorkerPollMs));
    }
}
```

- [ ] **Step 5: Register the source and the test**

In `CMakeLists.txt`, inside `add_library(obs-zoom-plugin MODULE ...)`, next to the Task 4 entry:

```cmake
        src/subject-detector-engine.cpp
```

Inside `if(BUILD_TESTING)`:

```cmake
    # The detector worker, driven against a FAKE ISubjectDetector -- which is
    # what the narrow interface bought us. The load-bearing assertion is the
    # thread-identity one: detection must never run on the thread that offered
    # the frame, because in production that is the shared engine-IPC reader
    # that dispatches for every source in the plugin, and nothing else in this
    # suite would notice it being violated (the symptom is the whole plugin
    # getting slower at once -- the 2026-08-17 signature). Links the real
    # engine .cpp but NOT libfacedetection: the fake factory means the CNN is
    # never constructed, so the weights blob stays out of this link.
    add_executable(CoreVideoSubjectWorkerTest
        tests/subject-worker-test.cpp
        src/subject-detector-engine.cpp
    )
    target_include_directories(CoreVideoSubjectWorkerTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoSubjectWorkerTest PRIVATE
        Threads::Threads libfacedetection)
    add_test(NAME CoreVideoSubjectWorker
             COMMAND CoreVideoSubjectWorkerTest)
```

(`libfacedetection` is linked because `subject-detector-engine.cpp` names
`make_facedetect_cnn_detector` in its default branch, which pulls in
`subject-detector-fd.cpp`'s symbol. The fake factory means it is never called,
so the test still measures only our own code — but the link needs to resolve.
Add `src/subject-detector-fd.cpp` to the `add_executable` list above if the
link reports the symbol unresolved.)

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release -R CoreVideoSubjectWorker --output-on-failure
```

Expected: PASS, `subject-worker OK (N detections)`.

- [ ] **Step 7: Tap the frame path in the Tiles source**

In `src/zoom-supersource.cpp`, add the include near the other project includes at the top of the file:

```cpp
#include "subject-detector-engine.h"
```

In `tile_feed_on_frame`, immediately after `feed->has_frame = true;` (currently the last line of the function, around line 443), add:

```cpp
    // Hand the detector one frame, and ONLY when it has asked for one.
    //
    // This runs on the engine-IPC reader thread that dispatches frames for
    // every source in the plugin, so the cost has to be bounded by the
    // DETECTOR'S schedule and not by the frame rate: offer_frame() is a single
    // atomic load that declines for every participant except the one the
    // worker is currently waiting on, which at a 100 ms tick is at most ten
    // acceptances per second across the whole wall. An accepted offer copies
    // one frame (~150 KB at 360p) and nothing else -- no detection, no colour
    // convert, no allocation beyond the copy. Detection itself happens on the
    // detector's own thread; see the threading contract on
    // src/subject-detector-engine.h.
    //
    // feed->mtx is already held here and the copy happens inside it, which is
    // correct: `frame` is the buffer tile_take_snapshot swaps out from under
    // us, so reading it unlocked would race the graphics thread.
    SubjectDetectorEngine::instance().offer_frame(
        feed->slot.participant_id(), feed->frame.data(), feed->frame.size(),
        w, h, feed->frame_epoch);
```

- [ ] **Step 8: Tell the engine who is monitored and who is speaking**

Still in `src/zoom-supersource.cpp`, in `tiles_video_render` (the function containing the `ctx->render_feeds = ctx->feeds;` snapshot around line 1286), immediately after that snapshot block closes, add:

```cpp
    // Keep the subject detector's monitored set equal to what the wall is
    // actually showing, and poll the DIRECTED active speaker for its schedule
    // boost.
    //
    // Pushed from here rather than the detector polling for itself because
    // ZoomEngineClient::roster() deep-copies strings under a hot mutex and must
    // never be called per frame -- the wall already knows its own assignment
    // set, for free. The push is rate-limited to once a second because
    // set_monitored() takes a lock and rebuilds the schedule's roster, and the
    // set changes on operator action, not per frame.
    //
    // SpeakerDirector is poll-only (no observer list), so the speaker is a
    // closure the worker calls once per tick rather than an event we forward.
    {
        static uint64_t s_last_monitored_push_ns = 0;
        const uint64_t now_ns = os_gettime_ns();
        if (now_ns - s_last_monitored_push_ns > 1000000000ull) {
            s_last_monitored_push_ns = now_ns;
            std::vector<uint32_t> ids;
            ids.reserve(feeds.size());
            for (const TileFeedPtr &f : feeds) {
                const uint32_t pid = f ? f->slot.participant_id() : 0;
                if (pid != 0) ids.push_back(pid);
            }
            SubjectDetectorEngine &det = SubjectDetectorEngine::instance();
            det.set_monitored(ids);
            if (!det.running()) {
                det.set_speaker_poll([] {
                    return ZoomEngineClient::instance().active_speaker_id();
                });
                det.start();
            }
        }
    }
```

Then stop the worker at plugin unload. In `src/plugin-main.cpp`'s `obs_module_unload()`, before the existing engine shutdown, add:

```cpp
    // Join the detector worker before anything it might still be reading goes
    // away. It holds no OBS or Zoom handles, so it can go first and its stop()
    // is idempotent.
    SubjectDetectorEngine::instance().stop();
```

with `#include "subject-detector-engine.h"` added to that file's includes.

- [ ] **Step 9: Build the plugin and run the whole suite**

```bash
cmake --build build --config Release --parallel 8
ctest -C Release --output-on-failure
```

Expected: N/N green, including all six new tests
(`CoreVideoFaceDetectLink`, `CoreVideoSubjectDetectorBench`,
`CoreVideoI420BgrDownscale`, `CoreVideoSubjectDetectorRecord`,
`CoreVideoSubjectSchedule`, `CoreVideoSubjectSmoothing`,
`CoreVideoSubjectInbox`, `CoreVideoSubjectWorker` — eight in total).

- [ ] **Step 10: Record the subsystem in CLAUDE.md**

Add to `CLAUDE.md`, in the "Invariants that have each caused a live-show defect" list:

```markdown
- **Subject detection never runs on the engine-IPC reader thread**
  (`src/subject-detector-engine.h`, tapped at `tile_feed_on_frame` in
  `src/zoom-supersource.cpp`): that thread dispatches frames for EVERY source
  in the plugin, and inline media work on it is the 2026-08-17 incident
  recorded on `src/media-event-queue.h` (~92% audio loss on every source). The
  producer's path there is one atomic load per frame plus, at most once per
  schedule tick, one ~150 KB copy into `SubjectFrameInbox`
  (`src/subject-frame-inbox.h`); the CNN runs on the detector's own worker
  thread. The cost ceiling is set by the SCHEDULE, not the roster
  (`src/subject-schedule.h`): one detection per ~100 ms cycled round-robin with
  an active-speaker boost, so a 24-person panel costs what a 2-person mic check
  costs. Consumers read `SubjectDetectorEngine::subject(id)`, which is already
  smoothed (`src/subject-smoothing.h`: deadband, rate limit, 1.5 s dropout
  hold) — so `found == false` from it means "gone for longer than the hold",
  never "missed one detection". `SubjectFrame` (`src/subject-frame.h`) is a
  published contract consumed by Tiles auto-framing and the return-feed
  overlay; do not rename its fields.
```

- [ ] **Step 11: Commit**

```bash
git add src/subject-detector-engine.h src/subject-detector-engine.cpp \
        tests/subject-worker-test.cpp src/zoom-supersource.cpp \
        src/plugin-main.cpp CMakeLists.txt CLAUDE.md
git commit -m "feat: run subject detection on a dedicated worker thread, off the IPC reader"
```

---

## What this plan does NOT deliver

Stated so the next author does not go looking:

- **Tiles auto-framing (Subsystem 3a).** Nothing here computes a crop rect or
  touches `solve_slot_crop` at `src/zoom-supersource.cpp:1957`. The spec's open
  question about tile source resolution (P360 by default vs. crisp auto-framed
  tiles, and the 2026-08-17 subscription-envelope throttle) gates that plan,
  not this one — detection is unaffected because we downscale to ~320 px
  regardless.
- **The return scene and framing overlay (Subsystem 3b).** No advice strings,
  no overlay rendering, no `corevideo_active_speaker_source` changes.
- **The loudness engine and meter (Subsystem 1).** Entirely separate; the two
  engines share nothing.
- **Self-tile exclusion.** The spec is emphatic that this is real, unwritten
  work and that a vcam return feed makes a feedback loop likely. It belongs to
  Subsystem 3b, which is the plan that adds the return feed. Detection running
  on the bot's own tile costs one schedule slot and produces a `SubjectFrame`
  nobody reads.
- **Confirmation that the detector finds a real face.** Every test here is pure
  CPU logic, per the repo's rule that no headless GPU harness exists and one
  has been ruled against. `CoreVideoFaceDetectLink` proves the library runs and
  `CoreVideoSubjectDetectorRecord` proves the arithmetic; that a real panelist
  in a real 360p Zoom tile is detected is a live check, and the first consumer
  plan is where it gets made.

## Self-review

**Spec coverage (Subsystem 2 only):**

| Spec requirement | Task |
|---|---|
| Vendor libfacedetection, BSD-3, four upstream files + our export header | 1 |
| BSD-3 attribution notice | 1 (`docs/THIRD-PARTY-NOTICES.md`) |
| No `/openmp`; `/arch:AVX2` provably on the compile line | 1 (CMakeLists + `corevideo-avx2-assert.cpp`, proved by deliberately breaking it in Step 9) |
| Local benchmarking before finalising the tick rate | 2, with the go/no-go table and the tick-rate consequence written down before the number is known |
| Input must be BGR 3-channel, converted at the downscaled size, not greyscale | 3 |
| ~320 px long edge | 3 (`kSubjectLongEdge`) |
| `SubjectFrame` exactly as the spec defines it | 4 |
| Five landmarks (eyes used; nose and mouth corners parsed but not surfaced) | 4 — the spec's framing rules need the eyeline, which is the eye pair; the other three landmarks are in the record and can be surfaced without a contract change if a consumer needs them |
| Narrow interface so the OpenCV-DNN fallback is a contained swap | 4 (`ISubjectDetector`, one method, one factory, one .cpp includes the CNN) |
| Round-robin, O(1) in participant count, active-speaker boost via the directed id | 5 |
| Test proving per-second detections stay fixed from 2 to 20 participants | 5, first assertion |
| Detection never on the engine-IPC reader thread; one dedicated worker | 7 (the handoff) and 8 (the thread, with a thread-identity assertion) |
| Temporal smoothing: deadband, rate limit, dropout hold | 6 |
| Test that a brief dropout does not flip to "no subject" | 6, dropout-hold block |
| Test that jitter below the deadband produces zero movement | 6, deadband block (asserts exact equality, not "small") |
| Plain `int main()` + local `check()`, `CoreVideo<Thing>Test` / `CoreVideo<Thing>`, hand-registered | every task |
| Everything testable as pure CPU logic, no GPU harness | every task; the only test that touches the vendored CNN at all is the link smoke test and the benchmark, both CPU |

**Placeholder scan:** no "TBD", no "add error handling", no "similar to Task N". The two fill-in-the-blank spots are deliberate and are *measurements the engineer must take*, not decisions deferred: the upstream commit SHA in Task 1 Step 1 and the benchmark table in Task 2 Step 5. Both have the exact command that produces the value.

**Type consistency check:** `SubjectFrame` field names are identical in `src/subject-frame.h`, the smoother, the record adapter, and every test. `kSubjectMinConfidencePct` is defined once (`subject-detector-fd-record.h`) and used by the adapter and the engine's default factory. `kSubjectLongEdge` is defined once (`i420-bgr-downscale.h`) and used by the worker and the benchmark's chosen sizes. `SubjectScheduleConfig::tick_ns` is the single tick knob, referenced by Task 2's verdict table, Task 5's header, and Task 8's request timeout. `i420_to_bgr_downscale`'s signature is the same in Task 3's header, Task 3's test, and Task 8's worker loop. `ISubjectDetector::detect` has the same four parameters in the interface, the real adapter, the fake in the worker test, and the worker's call site.
