# FFmpeg Runtime Download Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an operator press one button in the ISO Recorder dock to fetch a pinned, hash-verified FFmpeg build into CoreVideo's per-user folder, so ISO recording works on Windows and Apple Silicon macOS without a manual FFmpeg install. Fix the macOS FFmpeg picker and encoder list at the same time.

**Architecture:** The decisions live in Qt- and OBS-free headers that the host tests cover:
- the pins: URL, hash and archive layout per platform;
- the resolution order and archive-entry safety;
- the install-folder layout and swap.

A Qt singleton downloads, verifies, extracts with the OS's own unzip tool, test-runs the binary and swaps it in. The ISO recorder resolves FFmpeg through one glue function, so the dock, the control API and OSC all agree. The dock gains a status line and Download / Cancel / Remove.

**Tech Stack:** C++17, Qt 6 (Core, Network, Widgets), libobs (`obs_module_config_path`, `obs_module_file`), std::filesystem, CMake + plain-executable tests (`check()` style, no framework).

**Spec:** `docs/superpowers/specs/2026-10-03-ffmpeg-runtime-download-design.md`

## Global Constraints

- Platforms: Windows x64 and macOS Apple Silicon (arm64). Intel macOS is out of scope (no pin; it falls back to manual/PATH).
- FFmpeg variant: GPL, version **9.0.2** on both platforms.
- Windows pin: URL `https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip`; SHA-256 `60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba`; size `114768076`; archive entry `ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe`.
- macOS pin: URL `https://ffmpeg.martin-riedl.de/download/macos/arm64/1789931890_9.0.2/ffmpeg.zip`; SHA-256 `c8ed4c4e6978a03c485edbfe4e0a5dc2380f8a30bba5150531b31b094492d924`; size `28395699`; archive entry `ffmpeg`.
- The compiled-in hash is the only authority. Never fetch or trust an upstream `.sha256` at runtime.
- Downloads happen ONLY on an explicit operator click: Download, or Download in the first-use dialog. Never during install, never at startup.
- Install root: `obs_module_config_path("ffmpeg")`, which is per-user and needs no elevation. Layout `<root>/current/{ffmpeg|ffmpeg.exe, LICENSE.txt, provenance.txt}`; scratch paths `<root>/staging`, `<root>/previous`, `<root>/extract`, `<root>/download.part`.
- License: both builds are `--enable-gpl --enable-version3` (GPLv3). The macOS zip contains no license file, so CoreVideo ships the canonical GPLv3 text at `data/ffmpeg/LICENSE-GPLv3.txt` (from `https://www.gnu.org/licenses/gpl-3.0.txt`, SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`, 35149 bytes) and installs it as `LICENSE.txt` for both platforms. *(Approved spec said "copies the license from the archive"; this is the only deviation, forced by the macOS archive.)*
- Extraction tools: Windows `%SystemRoot%\System32\tar.exe` (list `-tf`, extract `-xf … -C`); macOS `/usr/bin/zipinfo -1` (list) and `/usr/bin/ditto -x -k` (extract).
- `QProcess` is allowed on the UI thread and on worker threads using `waitFor*`, but NEVER on media threads (CLAUDE.md, ISO ffmpeg feed).
- Resolution order: an explicit operator path that exists, then the managed install, then PATH, then (macOS only) `/opt/homebrew/bin/ffmpeg` and `/usr/local/bin/ffmpeg`.
- Operator copy: one plain line per failure, always ending in "Nothing was changed." when the existing setup is untouched.
- Engine-free change: plugin DLL only. CLAUDE.md is updated in the same change (standing rule).

## Review Focus

1. **The operator's saved FFmpeg path no longer exists** (FFmpeg uninstalled or the drive renamed). Resolution must fall through to managed/PATH/Homebrew, and the status must say "Configured FFmpeg not found; using …" rather than failing silently or using a ghost path. Test in Task 2.
2. **A previous download died part-way** (OBS crashed, laptop closed) and left `download.part`, `staging/`, `extract/` or `previous/` behind. The next attempt must clean these up first. A leftover `previous/` must never be mistaken for, or deleted instead of, a good `current/`. Test in Task 3.
3. **The profile path contains spaces or non-ASCII characters** (`C:\Users\José Müller\…`). The install, swap and completeness checks must work. Extraction must pass paths as `QProcess` argument lists, never as a joined command string. Test in Task 3 (filesystem); argument-list rule in Task 6.
4. **The operator double-clicks Download, or presses Start ISO during a download.** Exactly one download may be in flight. Start shows "FFmpeg is still downloading" and does not open the first-use dialog again. Covered in Task 6 (busy guard) and Task 7 (start gate).
5. **The operator presses Remove while an ISO recording runs on the managed FFmpeg.** Windows can't delete a running exe and macOS would yank it mid-recording. Remove must be disabled while recording, and `remove_install` must refuse with a message if it is reached anyway. Covered in Task 3 (refusal) and Task 7 (button state).

---

## File Structure

| File | Responsibility |
|---|---|
| `src/ffmpeg-runtime-pins.h` (new) | Pinned upstream archives per platform, plus `ffmpeg_runtime_pin_for_host()`. Pure. |
| `src/ffmpeg-runtime-plan.h` (new) | Resolution order, archive-entry safety, download size rule, provenance text. Pure. |
| `src/ffmpeg-runtime-fs.h/.cpp` (new) | Install-folder layout: completeness, leftover cleanup, swap, remove. std::filesystem only. |
| `src/ffmpeg-runtime-locate.h/.cpp` (new) | OBS/Qt glue: install root, managed exe path, `cv_ffmpeg_resolve()` for the real process. |
| `src/ffmpeg-runtime-installer.h/.cpp` (new) | Qt singleton: download, hash, extract, test-run, swap, cancel, remove. |
| `data/ffmpeg/LICENSE-GPLv3.txt` (new) | Canonical GPLv3 text installed next to the managed ffmpeg. |
| `src/iso-encoder-plan.h` (modify) | VideoToolbox in availability, automatic choice and demotion. |
| `src/zoom-iso-recorder.cpp` (modify) | Resolve FFmpeg via `cv_ffmpeg_resolve`, accept and probe `h264_videotoolbox`. |
| `src/zoom-iso-panel.h/.cpp` (modify) | Status line, Download / progress / Cancel / Remove, first-use dialog, macOS picker and encoder menu. |
| `tests/ffmpeg-runtime-pins-test.cpp`, `tests/ffmpeg-runtime-plan-test.cpp`, `tests/ffmpeg-runtime-fs-test.cpp` (new) | Host tests. |
| `tests/ffmpeg-runtime-live-test.cpp` (new) | Manual network test: the pinned URL still hashes to the pin. Built, not in ctest. |
| `tests/iso-encoder-plan-test.cpp` (modify) | VideoToolbox cases. |
| `CMakeLists.txt` (modify) | Plugin sources, new tests. |
| `docs/policies/privacy-policy.md`, `docs/OPERATOR_QUICKSTART.md`, `THIRD-PARTY-NOTICES.md` (new), `CLAUDE.md`, `CHANGELOG.md` (modify) | Docs. |

---

### Task 1: Pins and bundled license

**Files:**
- Create: `src/ffmpeg-runtime-pins.h`
- Create: `data/ffmpeg/LICENSE-GPLv3.txt`
- Create: `tests/ffmpeg-runtime-pins-test.cpp`
- Modify: `CMakeLists.txt` (add test next to `CoreVideoIsoEncoderPlanTest`, around line 673)

**Interfaces:**
- Produces: `struct FfmpegRuntimePin { const char *platform, *version, *host, *url, *sha256; std::uint64_t size_bytes; const char *archive_exe, *exe_name; };`, `kFfmpegPinWindowsX64`, `kFfmpegPinMacosArm64`, `const FfmpegRuntimePin *ffmpeg_runtime_pin_for_host();` (nullptr on unsupported hosts), and `constexpr const char *kFfmpegLicenseDataFile = "ffmpeg/LICENSE-GPLv3.txt";`

- [ ] **Step 1: Write the failing test** — `tests/ffmpeg-runtime-pins-test.cpp`:

```cpp
// The pinned FFmpeg archives are the ONLY authority for what CoreVideo will
// run as ISO FFmpeg (spec 2026-10-03). A blank field or a malformed hash ships
// a Download button that can never succeed, so pin completeness is checked here.
#include "ffmpeg-runtime-pins.h"

#include <cctype>
#include <cstring>
#include <iostream>

static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

static bool lower_hex64(const char *s)
{
    if (!s || std::strlen(s) != 64) return false;
    for (const char *p = s; *p; ++p)
        if (!(std::isdigit(static_cast<unsigned char>(*p)) || (*p >= 'a' && *p <= 'f')))
            return false;
    return true;
}

static void check_pin(const FfmpegRuntimePin &p, const char *name)
{
    std::cerr << "pin " << name << "\n";
    check(p.platform && *p.platform, "platform set");
    check(p.version && *p.version, "version set");
    check(p.host && *p.host, "host set");
    check(p.url && std::strncmp(p.url, "https://", 8) == 0, "url is https");
    check(lower_hex64(p.sha256), "sha256 is 64 lowercase hex chars");
    check(p.size_bytes > 1000000, "size is plausible");
    check(p.archive_exe && *p.archive_exe, "archive_exe set");
    check(p.exe_name && *p.exe_name, "exe_name set");
}

int main()
{
    check_pin(kFfmpegPinWindowsX64, "windows");
    check_pin(kFfmpegPinMacosArm64, "macos");
    check(std::strcmp(kFfmpegPinWindowsX64.version, kFfmpegPinMacosArm64.version) == 0,
          "both platforms pin the same FFmpeg version");
    check(std::strcmp(kFfmpegPinWindowsX64.exe_name, "ffmpeg.exe") == 0, "windows exe name");
    check(std::strcmp(kFfmpegPinMacosArm64.exe_name, "ffmpeg") == 0, "macos exe name");
    check(std::strcmp(kFfmpegLicenseDataFile, "ffmpeg/LICENSE-GPLv3.txt") == 0, "license data path");
#if defined(_WIN32)
    check(ffmpeg_runtime_pin_for_host() == &kFfmpegPinWindowsX64, "host pin is windows");
#endif
    if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
    std::cout << "ffmpeg runtime pins: all checks passed\n";
    return 0;
}
```

Register it in `CMakeLists.txt` directly after the `CoreVideoIsoEncoderPlan` `add_test` block:

```cmake
    # The pinned FFmpeg runtime archives (spec 2026-10-03): every field set,
    # well-formed hash, one version across platforms.
    add_executable(CoreVideoFfmpegRuntimePinsTest tests/ffmpeg-runtime-pins-test.cpp)
    target_include_directories(CoreVideoFfmpegRuntimePinsTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME CoreVideoFfmpegRuntimePins COMMAND CoreVideoFfmpegRuntimePinsTest)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimePinsTest`
Expected: compile error `ffmpeg-runtime-pins.h: No such file or directory`.

- [ ] **Step 3: Write the header** — `src/ffmpeg-runtime-pins.h`:

```cpp
#pragma once
// The pinned upstream FFmpeg builds CoreVideo downloads for ISO recording when
// the operator asks (spec docs/superpowers/specs/2026-10-03-ffmpeg-runtime-
// download-design.md). Both are GPLv3 builds (--enable-gpl --enable-version3)
// that include libx264, the always-available last step of the ISO encoder
// fallback chain. The hash here is the ONLY authority: an upstream .sha256 is
// never fetched or trusted at runtime.
//
// To move to a newer FFmpeg: change both entries together (one version on
// both platforms), then run CoreVideoFfmpegRuntimeLiveTest on each platform.
// It downloads the real archive and fails if it no longer hashes to the pin.
// Pure: no Qt, no OBS.
#include <cstdint>

struct FfmpegRuntimePin {
    const char *platform;      // "windows-x64" / "macos-arm64"
    const char *version;       // shown to the operator
    const char *host;          // shown in the download prompt
    const char *url;
    const char *sha256;        // lowercase hex of the whole archive
    std::uint64_t size_bytes;  // exact archive size
    const char *archive_exe;   // path of the executable inside the zip
    const char *exe_name;      // file name installed under <root>/current
};

// gyan.dev GPL "essentials" build. Versioned GitHub releases are kept long
// term (7.1 from 2024 is still downloadable as of 2026-10-03).
inline constexpr FfmpegRuntimePin kFfmpegPinWindowsX64{
    "windows-x64",
    "9.0.2",
    "github.com (gyan.dev builds)",
    "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
    "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
    114768076ULL,
    "ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe",
    "ffmpeg.exe",
};

// Martin Riedl static build, signed. Includes libx264 and h264_videotoolbox.
// The zip holds ONLY the binary (no license), hence kFfmpegLicenseDataFile.
inline constexpr FfmpegRuntimePin kFfmpegPinMacosArm64{
    "macos-arm64",
    "9.0.2",
    "ffmpeg.martin-riedl.de",
    "https://ffmpeg.martin-riedl.de/download/macos/arm64/1789931890_9.0.2/ffmpeg.zip",
    "c8ed4c4e6978a03c485edbfe4e0a5dc2380f8a30bba5150531b31b094492d924",
    28395699ULL,
    "ffmpeg",
    "ffmpeg",
};

// Canonical GPLv3 text shipped in the plugin's data dir and installed as
// LICENSE.txt beside the managed ffmpeg on every platform.
constexpr const char *kFfmpegLicenseDataFile = "ffmpeg/LICENSE-GPLv3.txt";

// The pin for the platform this binary was built for; nullptr where CoreVideo
// offers no managed FFmpeg (Intel macOS, Linux). Callers then fall back to
// PATH / a manually chosen ffmpeg.
inline const FfmpegRuntimePin *ffmpeg_runtime_pin_for_host()
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return &kFfmpegPinWindowsX64;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return &kFfmpegPinMacosArm64;
#else
    return nullptr;
#endif
}
```

- [ ] **Step 4: Add the license text**

Run: `mkdir -p data/ffmpeg && curl -sSL -o data/ffmpeg/LICENSE-GPLv3.txt https://www.gnu.org/licenses/gpl-3.0.txt && sha256sum data/ffmpeg/LICENSE-GPLv3.txt`
Expected: `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`. Any other hash means the download is wrong; stop. `install(DIRECTORY data/ …)` at CMakeLists.txt:580 already ships everything under `data/`, so no CMake change is needed for packaging.

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake -S . -B build_x64 >/dev/null && cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimePinsTest && build_x64/Release/CoreVideoFfmpegRuntimePinsTest.exe`
Expected: `ffmpeg runtime pins: all checks passed`.

- [ ] **Step 6: Commit**

```bash
git add src/ffmpeg-runtime-pins.h data/ffmpeg/LICENSE-GPLv3.txt tests/ffmpeg-runtime-pins-test.cpp CMakeLists.txt
git commit -m "FFmpeg runtime: pinned upstream archives and bundled GPLv3 text"
```

---

### Task 2: Resolution order, archive-entry safety, size rule, provenance

**Files:**
- Create: `src/ffmpeg-runtime-plan.h`
- Create: `tests/ffmpeg-runtime-plan-test.cpp`
- Modify: `CMakeLists.txt` (after Task 1's test block)

**Interfaces:**
- Consumes: `FfmpegRuntimePin` (Task 1).
- Produces:
  - `enum class FfmpegSource { Explicit, Managed, Path, Homebrew, None };`
  - `struct FfmpegResolveInputs { std::string configured; std::string managed_exe; bool managed_complete = false; bool macos = false; std::function<bool(const std::string &)> file_exists; std::function<std::string(const std::string &)> find_on_path; };`
  - `struct FfmpegResolution { std::string path; FfmpegSource source = FfmpegSource::None; bool configured_missing = false; };`
  - `bool ffmpeg_is_default_name(const std::string &)`
  - `FfmpegResolution ffmpeg_resolve(const FfmpegResolveInputs &)`
  - `bool ffmpeg_archive_entry_safe(const std::string &entry)`
  - `bool ffmpeg_download_size_ok(std::uint64_t received, std::uint64_t expected)`
  - `std::string ffmpeg_provenance_text(const FfmpegRuntimePin &, const std::string &utc_iso8601)`

- [ ] **Step 1: Write the failing test** — `tests/ffmpeg-runtime-plan-test.cpp`:

```cpp
// Which FFmpeg the ISO recorder runs, and which archive entries the installer
// will touch (spec 2026-10-03). The resolution order is what makes a Download
// button "just work" without overriding an operator's own choice.
#include "ffmpeg-runtime-plan.h"

#include <iostream>
#include <set>
#include <string>

static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

static FfmpegResolveInputs base(std::set<std::string> files, std::string on_path = "")
{
    FfmpegResolveInputs in;
    in.file_exists = [files](const std::string &p) { return files.count(p) != 0; };
    in.find_on_path = [on_path](const std::string &) { return on_path; };
    return in;
}

int main()
{
    const std::string managed = "/home/u/cfg/ffmpeg/current/ffmpeg";

    // Default names mean "not chosen by the operator".
    check(ffmpeg_is_default_name(""), "empty is default");
    check(ffmpeg_is_default_name("ffmpeg"), "ffmpeg is default");
    check(ffmpeg_is_default_name("ffmpeg.exe"), "ffmpeg.exe is default");
    check(ffmpeg_is_default_name("  ffmpeg  "), "trimmed default");
    check(!ffmpeg_is_default_name("/opt/ff/ffmpeg"), "absolute path is a choice");

    // 1. Explicit operator path that exists wins over everything.
    {
        auto in = base({"/opt/ff/ffmpeg", managed}, "/usr/bin/ffmpeg");
        in.configured = "/opt/ff/ffmpeg";
        in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Explicit && r.path == "/opt/ff/ffmpeg", "explicit wins");
        check(!r.configured_missing, "explicit present is not missing");
    }
    // An explicit path equal to the managed exe reports Managed (the dock
    // writes the managed path into the field after a download).
    {
        auto in = base({managed});
        in.configured = managed; in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Managed && r.path == managed, "configured==managed reports Managed");
    }
    // REVIEW FOCUS 1: a saved path that vanished falls through AND says so.
    {
        auto in = base({managed});
        in.configured = "D:/tools/ffmpeg.exe";
        in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Managed && r.path == managed, "missing explicit falls to managed");
        check(r.configured_missing, "missing explicit is reported");
    }
    // 2. Managed beats PATH when nothing explicit is set.
    {
        auto in = base({managed}, "/usr/bin/ffmpeg");
        in.configured = "ffmpeg"; in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Managed, "managed beats PATH");
    }
    // An incomplete managed install (no license/provenance) does not count.
    {
        auto in = base({managed}, "/usr/bin/ffmpeg");
        in.configured = "ffmpeg"; in.managed_exe = managed; in.managed_complete = false;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Path && r.path == "/usr/bin/ffmpeg", "incomplete managed is skipped");
    }
    // 3. PATH.
    {
        auto in = base({}, "C:/ffmpeg/bin/ffmpeg.exe");
        in.configured = "ffmpeg";
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Path && r.path == "C:/ffmpeg/bin/ffmpeg.exe", "PATH found");
    }
    // A relative non-default name is looked up on PATH and counts as Explicit.
    {
        auto in = base({}, "/usr/bin/ffmpeg7");
        in.configured = "ffmpeg7";
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Explicit && r.path == "/usr/bin/ffmpeg7", "relative explicit via PATH");
    }
    // 4. macOS Homebrew fallback, Apple Silicon prefix first, only on macOS.
    {
        auto in = base({"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"});
        in.configured = "ffmpeg"; in.macos = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Homebrew && r.path == "/opt/homebrew/bin/ffmpeg", "homebrew arm prefix first");
    }
    {
        auto in = base({"/usr/local/bin/ffmpeg"});
        in.configured = "ffmpeg"; in.macos = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Homebrew && r.path == "/usr/local/bin/ffmpeg", "homebrew intel prefix");
    }
    {
        auto in = base({"/opt/homebrew/bin/ffmpeg"});
        in.configured = "ffmpeg"; in.macos = false;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::None && r.path.empty(), "no homebrew lookup off macOS");
    }

    // Archive-entry safety: anything that could land outside the staging root.
    check(ffmpeg_archive_entry_safe("ffmpeg"), "plain entry ok");
    check(ffmpeg_archive_entry_safe("ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe"), "nested entry ok");
    check(ffmpeg_archive_entry_safe("a/b/"), "directory entry ok");
    check(!ffmpeg_archive_entry_safe(""), "empty rejected");
    check(!ffmpeg_archive_entry_safe("/etc/passwd"), "absolute rejected");
    check(!ffmpeg_archive_entry_safe("\\Windows\\x.dll"), "backslash-rooted rejected");
    check(!ffmpeg_archive_entry_safe("C:/x.exe"), "drive letter rejected");
    check(!ffmpeg_archive_entry_safe("c:x.exe"), "drive-relative rejected");
    check(!ffmpeg_archive_entry_safe("../x"), "parent segment rejected");
    check(!ffmpeg_archive_entry_safe("a/../../x"), "nested parent rejected");
    check(!ffmpeg_archive_entry_safe("a\\..\\x"), "backslash parent rejected");
    check(ffmpeg_archive_entry_safe("a/..b/x"), "'..b' is a name, not a parent");

    // Download size rule: abort past expected + 10%.
    check(ffmpeg_download_size_ok(100, 100), "exact ok");
    check(ffmpeg_download_size_ok(110, 100), "+10% ok");
    check(!ffmpeg_download_size_ok(111, 100), "over +10% aborts");

    // Provenance text names source, hash, version, time and the redistribution note.
    {
        const std::string t = ffmpeg_provenance_text(kFfmpegPinWindowsX64, "2026-10-03T12:00:00Z");
        check(t.find(kFfmpegPinWindowsX64.url) != std::string::npos, "provenance has url");
        check(t.find(kFfmpegPinWindowsX64.sha256) != std::string::npos, "provenance has hash");
        check(t.find("9.0.2") != std::string::npos, "provenance has version");
        check(t.find("2026-10-03T12:00:00Z") != std::string::npos, "provenance has time");
        check(t.find("not redistributed by CoreVideo") != std::string::npos, "provenance has redistribution note");
    }

    if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
    std::cout << "ffmpeg runtime plan: all checks passed\n";
    return 0;
}
```

CMake, after Task 1's block:

```cmake
    add_executable(CoreVideoFfmpegRuntimePlanTest tests/ffmpeg-runtime-plan-test.cpp)
    target_include_directories(CoreVideoFfmpegRuntimePlanTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME CoreVideoFfmpegRuntimePlan COMMAND CoreVideoFfmpegRuntimePlanTest)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build_x64 >/dev/null && cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimePlanTest`
Expected: compile error, `ffmpeg-runtime-plan.h` not found.

- [ ] **Step 3: Write the header** — `src/ffmpeg-runtime-plan.h`:

```cpp
#pragma once
// Decisions behind the managed FFmpeg (spec 2026-10-03): which ffmpeg the ISO
// recorder runs, which archive entries the installer may touch, when a
// download is oversized, and what provenance.txt says. Pure: the filesystem
// and PATH are injected so every branch is host-testable.
#include "ffmpeg-runtime-pins.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>

enum class FfmpegSource { Explicit, Managed, Path, Homebrew, None };

struct FfmpegResolveInputs {
    std::string configured;   // dock field / saved setting / API ffmpeg_path
    std::string managed_exe;  // <root>/current/<exe_name>, "" if no pin
    bool managed_complete = false;
    bool macos = false;
    std::function<bool(const std::string &)> file_exists;
    std::function<std::string(const std::string &)> find_on_path;  // "" = not found
};

struct FfmpegResolution {
    std::string path;
    FfmpegSource source = FfmpegSource::None;
    // The operator chose a path that no longer exists, and resolution fell
    // through to something else. The dock must say so: silently using a
    // different binary is how "my FFmpeg setting is ignored" reports start.
    bool configured_missing = false;
};

inline std::string ffmpeg_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// The settings default ("ffmpeg") and its Windows spelling mean "nothing
// chosen" -- resolution may pick the managed copy over them.
inline bool ffmpeg_is_default_name(const std::string &configured)
{
    const std::string t = ffmpeg_trim(configured);
    return t.empty() || t == "ffmpeg" || t == "ffmpeg.exe";
}

inline bool ffmpeg_is_relative_name(const std::string &p)
{
    return p.find('/') == std::string::npos && p.find('\\') == std::string::npos &&
           !(p.size() >= 2 && p[1] == ':');
}

inline FfmpegResolution ffmpeg_resolve(const FfmpegResolveInputs &in)
{
    FfmpegResolution r;
    const std::string configured = ffmpeg_trim(in.configured);
    const bool managed_ok =
        in.managed_complete && !in.managed_exe.empty() && in.file_exists(in.managed_exe);

    if (!ffmpeg_is_default_name(configured)) {
        if (managed_ok && configured == in.managed_exe) {
            r.path = in.managed_exe;
            r.source = FfmpegSource::Managed;
            return r;
        }
        const std::string found = ffmpeg_is_relative_name(configured)
            ? in.find_on_path(configured)
            : (in.file_exists(configured) ? configured : std::string());
        if (!found.empty()) {
            r.path = found;
            r.source = FfmpegSource::Explicit;
            return r;
        }
        r.configured_missing = true;
    }
    if (managed_ok) {
        r.path = in.managed_exe;
        r.source = FfmpegSource::Managed;
        return r;
    }
    const std::string on_path = in.find_on_path("ffmpeg");
    if (!on_path.empty()) {
        r.path = on_path;
        r.source = FfmpegSource::Path;
        return r;
    }
    if (in.macos) {
        // OBS launched from the Dock does not inherit the shell's PATH, so
        // Homebrew's ffmpeg is invisible to findExecutable even when installed.
        for (const char *candidate : {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"}) {
            if (in.file_exists(candidate)) {
                r.path = candidate;
                r.source = FfmpegSource::Homebrew;
                return r;
            }
        }
    }
    return r;
}

// True when extracting `entry` cannot write outside the staging root.
inline bool ffmpeg_archive_entry_safe(const std::string &entry)
{
    if (entry.empty()) return false;
    if (entry[0] == '/' || entry[0] == '\\') return false;
    if (entry.size() >= 2 && entry[1] == ':') return false;
    size_t start = 0;
    while (start <= entry.size()) {
        size_t end = entry.find_first_of("/\\", start);
        if (end == std::string::npos) end = entry.size();
        if (entry.compare(start, end - start, "..") == 0 && end - start == 2)
            return false;
        start = end + 1;
    }
    return true;
}

// Abort a download that runs more than 10% past the pinned size.
inline bool ffmpeg_download_size_ok(std::uint64_t received, std::uint64_t expected)
{
    return received <= expected + expected / 10;
}

inline std::string ffmpeg_provenance_text(const FfmpegRuntimePin &pin,
                                          const std::string &utc_iso8601)
{
    std::string t;
    t += "FFmpeg " + std::string(pin.version) + " (" + pin.platform + ")\n";
    t += "Source: " + std::string(pin.url) + "\n";
    t += "SHA-256: " + std::string(pin.sha256) + "\n";
    t += "Installed: " + utc_iso8601 + "\n";
    t += "License: GPLv3 (see LICENSE.txt)\n";
    t += "Downloaded at the operator's request directly from upstream; "
         "not redistributed by CoreVideo.\n";
    return t;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimePlanTest && build_x64/Release/CoreVideoFfmpegRuntimePlanTest.exe`
Expected: `ffmpeg runtime plan: all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add src/ffmpeg-runtime-plan.h tests/ffmpeg-runtime-plan-test.cpp CMakeLists.txt
git commit -m "FFmpeg runtime: resolution order, archive-entry safety, provenance"
```

---

### Task 3: Install-folder layout (completeness, leftovers, swap, remove)

**Files:**
- Create: `src/ffmpeg-runtime-fs.h`, `src/ffmpeg-runtime-fs.cpp`
- Create: `tests/ffmpeg-runtime-fs-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces (namespace `cvff`, all paths `std::filesystem::path`):
  - `path current_dir(const path &root)`, `staging_dir(root)`, `previous_dir(root)`, `extract_dir(root)`, `download_file(root)`
  - `bool install_complete(const path &root, const std::string &exe_name)`: `current/` holds `exe_name`, `LICENSE.txt` and `provenance.txt`
  - `void clean_leftovers(const path &root)`: removes `staging/`, `extract/`, `download.part`; if `current/` is missing but `previous/` exists, renames `previous` back to `current` (a crash mid-swap), otherwise removes `previous/`
  - `bool swap_in_staging(const path &root, std::string *error)`
  - `bool remove_install(const path &root, bool in_use, std::string *error)`

- [ ] **Step 1: Write the failing test** — `tests/ffmpeg-runtime-fs-test.cpp`:

```cpp
// The managed-FFmpeg folder under the operator's OBS profile (spec
// 2026-10-03). The invariant every case here defends: a good install is never
// lost to a failed or interrupted update.
#include "ffmpeg-runtime-fs.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
static void write(const fs::path &p, const std::string &body)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
static std::string read(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void fill(const fs::path &dir, const std::string &tag)
{
    write(dir / "ffmpeg.exe", tag);
    write(dir / "LICENSE.txt", "gpl");
    write(dir / "provenance.txt", "prov");
}

int main()
{
    // REVIEW FOCUS 3: a profile path with spaces and non-ASCII characters.
    const fs::path root = fs::temp_directory_path() /
        fs::u8path(u8"cv ffmpeg test Jos\u00e9 M\u00fcller") / "ffmpeg";
    fs::remove_all(root.parent_path());

    check(!cvff::install_complete(root, "ffmpeg.exe"), "nothing installed yet");

    // First install: staging -> current.
    fill(cvff::staging_dir(root), "v1");
    std::string err;
    check(cvff::swap_in_staging(root, &err), "first swap succeeds");
    check(cvff::install_complete(root, "ffmpeg.exe"), "complete after first swap");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v1", "v1 in place");
    check(!fs::exists(cvff::staging_dir(root)), "staging consumed");
    check(!fs::exists(cvff::previous_dir(root)), "no previous left behind");

    // Update: v2 replaces v1, old copy removed.
    fill(cvff::staging_dir(root), "v2");
    check(cvff::swap_in_staging(root, &err), "update swap succeeds");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "v2 in place");
    check(!fs::exists(cvff::previous_dir(root)), "previous removed after update");

    // A swap with nothing staged fails AND leaves v2 intact.
    check(!cvff::swap_in_staging(root, &err), "swap without staging fails");
    check(!err.empty(), "failure explains itself");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "v2 survives failed swap");

    // An incomplete current (license missing) is not an install.
    fs::remove(cvff::current_dir(root) / "LICENSE.txt");
    check(!cvff::install_complete(root, "ffmpeg.exe"), "missing license => incomplete");
    write(cvff::current_dir(root) / "LICENSE.txt", "gpl");

    // REVIEW FOCUS 2: leftovers from an interrupted run are cleaned...
    write(cvff::download_file(root), "partial");
    fill(cvff::staging_dir(root), "half");
    write(cvff::extract_dir(root) / "x" / "ffmpeg.exe", "junk");
    write(cvff::previous_dir(root) / "ffmpeg.exe", "stale");
    cvff::clean_leftovers(root);
    check(!fs::exists(cvff::download_file(root)), "download.part removed");
    check(!fs::exists(cvff::staging_dir(root)), "staging removed");
    check(!fs::exists(cvff::extract_dir(root)), "extract removed");
    check(!fs::exists(cvff::previous_dir(root)), "stale previous removed when current exists");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "current untouched by cleanup");

    // ...and a crash between "current -> previous" and "staging -> current"
    // is recovered by restoring previous, never by deleting it.
    fs::rename(cvff::current_dir(root), cvff::previous_dir(root));
    cvff::clean_leftovers(root);
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "previous restored after mid-swap crash");
    check(cvff::install_complete(root, "ffmpeg.exe"), "restored install complete");

    // REVIEW FOCUS 5: remove refuses while the managed ffmpeg is in use.
    check(!cvff::remove_install(root, /*in_use=*/true, &err), "remove refused while in use");
    check(err.find("recording") != std::string::npos, "refusal mentions recording");
    check(cvff::install_complete(root, "ffmpeg.exe"), "refused remove leaves install");
    check(cvff::remove_install(root, false, &err), "remove succeeds when idle");
    check(!fs::exists(cvff::current_dir(root)), "current gone after remove");
    check(cvff::remove_install(root, false, &err), "remove with nothing installed is a no-op success");

    fs::remove_all(root.parent_path());
    if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
    std::cout << "ffmpeg runtime fs: all checks passed\n";
    return 0;
}
```

CMake:

```cmake
    add_executable(CoreVideoFfmpegRuntimeFsTest
        tests/ffmpeg-runtime-fs-test.cpp
        src/ffmpeg-runtime-fs.cpp)
    target_include_directories(CoreVideoFfmpegRuntimeFsTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME CoreVideoFfmpegRuntimeFs COMMAND CoreVideoFfmpegRuntimeFsTest)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build_x64 >/dev/null && cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimeFsTest`
Expected: compile error, `ffmpeg-runtime-fs.h` not found.

- [ ] **Step 3: Implement** — `src/ffmpeg-runtime-fs.h`:

```cpp
#pragma once
// The managed-FFmpeg folder layout (spec 2026-10-03):
//   <root>/current/{<exe>, LICENSE.txt, provenance.txt}   the live install
//   <root>/staging/   a verified candidate, swapped in atomically by rename
//   <root>/previous/  the old install during a swap (restored on failure)
//   <root>/extract/   raw unzip output, discarded
//   <root>/download.part
// std::filesystem only, so it is host-testable with temp folders.
#include <filesystem>
#include <string>

namespace cvff {
using path = std::filesystem::path;

inline path current_dir(const path &root) { return root / "current"; }
inline path staging_dir(const path &root) { return root / "staging"; }
inline path previous_dir(const path &root) { return root / "previous"; }
inline path extract_dir(const path &root) { return root / "extract"; }
inline path download_file(const path &root) { return root / "download.part"; }

bool install_complete(const path &root, const std::string &exe_name);
void clean_leftovers(const path &root);
bool swap_in_staging(const path &root, std::string *error);
bool remove_install(const path &root, bool in_use, std::string *error);
}
```

`src/ffmpeg-runtime-fs.cpp`:

```cpp
#include "ffmpeg-runtime-fs.h"

namespace fs = std::filesystem;

namespace cvff {

bool install_complete(const path &root, const std::string &exe_name)
{
    std::error_code ec;
    const path cur = current_dir(root);
    return fs::is_regular_file(cur / exe_name, ec) &&
           fs::is_regular_file(cur / "LICENSE.txt", ec) &&
           fs::is_regular_file(cur / "provenance.txt", ec);
}

void clean_leftovers(const path &root)
{
    std::error_code ec;
    fs::remove(download_file(root), ec);
    fs::remove_all(staging_dir(root), ec);
    fs::remove_all(extract_dir(root), ec);
    // A crash between the two renames in swap_in_staging leaves the good
    // install in previous/ and no current/. Restore it -- never delete it.
    if (!fs::exists(current_dir(root), ec) && fs::exists(previous_dir(root), ec))
        fs::rename(previous_dir(root), current_dir(root), ec);
    else
        fs::remove_all(previous_dir(root), ec);
}

bool swap_in_staging(const path &root, std::string *error)
{
    std::error_code ec;
    if (!fs::is_directory(staging_dir(root), ec)) {
        if (error) *error = "Nothing staged to install.";
        return false;
    }
    fs::remove_all(previous_dir(root), ec);
    const bool had_current = fs::exists(current_dir(root), ec);
    if (had_current) {
        fs::rename(current_dir(root), previous_dir(root), ec);
        if (ec) {
            if (error) *error = "Could not move the existing FFmpeg aside: " + ec.message();
            return false;
        }
    }
    fs::rename(staging_dir(root), current_dir(root), ec);
    if (ec) {
        const std::string why = ec.message();
        if (had_current) {
            std::error_code back;
            fs::rename(previous_dir(root), current_dir(root), back);
        }
        if (error) *error = "Could not install the new FFmpeg: " + why;
        return false;
    }
    fs::remove_all(previous_dir(root), ec);
    return true;
}

bool remove_install(const path &root, bool in_use, std::string *error)
{
    if (in_use) {
        if (error) *error = "Stop ISO recording before removing FFmpeg.";
        return false;
    }
    std::error_code ec;
    fs::remove_all(current_dir(root), ec);
    if (ec) {
        if (error) *error = "Could not remove FFmpeg: " + ec.message();
        return false;
    }
    clean_leftovers(root);
    return true;
}

}  // namespace cvff
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimeFsTest && build_x64/Release/CoreVideoFfmpegRuntimeFsTest.exe`
Expected: `ffmpeg runtime fs: all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add src/ffmpeg-runtime-fs.h src/ffmpeg-runtime-fs.cpp tests/ffmpeg-runtime-fs-test.cpp CMakeLists.txt
git commit -m "FFmpeg runtime: install layout with crash-safe swap and remove"
```

---

### Task 4: VideoToolbox encoder, and resolving FFmpeg in the recorder

**Files:**
- Modify: `src/iso-encoder-plan.h`
- Modify: `tests/iso-encoder-plan-test.cpp`
- Create: `src/ffmpeg-runtime-locate.h`, `src/ffmpeg-runtime-locate.cpp`
- Modify: `src/zoom-iso-recorder.cpp` (FFmpeg existence check ~lines 94-106; availability probe ~112-116; `normalized_video_encoder` ~478; `is_hardware_encoder` ~514)
- Modify: `CMakeLists.txt` (add the new plugin sources next to `src/zoom-iso-recorder.cpp` in the plugin target's source list)

**Interfaces:**
- Consumes: `ffmpeg_resolve`, `FfmpegResolution`, `FfmpegSource` (Task 2); `cvff::install_complete`, `cvff::current_dir` (Task 3); `ffmpeg_runtime_pin_for_host` (Task 1).
- Produces:
  - `IsoEncoderAvailability::videotoolbox` (bool)
  - `QString cv_ffmpeg_install_root()`: the managed root, created if missing
  - `QString cv_ffmpeg_managed_exe()`: the exe path, or empty when there is no pin
  - `bool cv_ffmpeg_managed_installed()`
  - `FfmpegResolution cv_ffmpeg_resolve(const std::string &configured)`

- [ ] **Step 1: Write the failing test** by appending to `tests/iso-encoder-plan-test.cpp`, before the final `if (g_failures)` block in `main()`:

```cpp
    // macOS (spec 2026-10-03): Apple VideoToolbox is the hardware path, and it
    // demotes straight to x264. The Windows chain is untouched.
    {
        IsoEncoderAvailability mac;
        mac.videotoolbox = true;
        check(iso_choose_session_encoder("auto", 8, mac) == "h264_videotoolbox",
              "auto picks VideoToolbox when available");
        check(iso_choose_session_encoder("libx264", 8, mac) == "libx264",
              "explicit x264 honored on mac");
        check(iso_choose_session_encoder("h264_videotoolbox", 0, mac) == "h264_videotoolbox",
              "explicit VideoToolbox honored");
        check(iso_demote_encoder("h264_videotoolbox", mac) == "libx264",
              "VideoToolbox demotes to x264");
        IsoEncoderAvailability win;
        win.nvenc = true;
        check(iso_choose_session_encoder("auto", 8, win) == "h264_nvenc",
              "windows auto unchanged (nvenc)");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build_x64 --config Release --target CoreVideoIsoEncoderPlanTest`
Expected: compile error, `'struct IsoEncoderAvailability' has no member named 'videotoolbox'`.

- [ ] **Step 3: Update `src/iso-encoder-plan.h`**

Add the field to the struct:

```cpp
struct IsoEncoderAvailability {
    bool nvenc = false;
    bool qsv = false;
    bool amf = false;
    // Apple VideoToolbox (macOS). No session budget we need to model; it
    // demotes straight to x264. Never true on Windows FFmpeg builds.
    bool videotoolbox = false;
};
```

In `iso_choose_session_encoder`, change the explicit-choice guard and add VideoToolbox as the first automatic choice:

```cpp
    if (!automatic && requested != "h264_nvenc") {
        // Explicit non-NVENC choice (QSV/AMF/VideoToolbox/x264): honored; no
        // shared budget to model. Absent hardware is handled at start.
        return requested;
    }

    // macOS: VideoToolbox is the only hardware encoder there.
    if (automatic && avail.videotoolbox)
        return "h264_videotoolbox";

    // "auto", or explicit NVENC: NVENC while the budget lasts.
```

`iso_demote_encoder` already returns `"libx264"` for any encoder it doesn't name. Add a comment line above its final `return "libx264";`: `// h264_videotoolbox, h264_amf, anything else: x264 cannot run out.`

- [ ] **Step 4: Run to verify the encoder test passes**

Run: `cmake --build build_x64 --config Release --target CoreVideoIsoEncoderPlanTest && build_x64/Release/CoreVideoIsoEncoderPlanTest.exe`
Expected: all checks pass, including the 5 new ones.

- [ ] **Step 5: Write the locate glue** — `src/ffmpeg-runtime-locate.h`:

```cpp
#pragma once
// OBS/Qt glue for the managed FFmpeg (spec 2026-10-03): where it lives for
// this OBS profile, and which ffmpeg the ISO recorder should run. The dock,
// the control API and OSC all start ISO through ZoomIsoRecorder::start(),
// which calls cv_ffmpeg_resolve() -- so they cannot disagree.
#include "ffmpeg-runtime-plan.h"

#include <QString>
#include <string>

QString cv_ffmpeg_install_root();          // obs_module_config_path("ffmpeg"), created
QString cv_ffmpeg_managed_exe();           // "" when this platform has no pin
bool cv_ffmpeg_managed_installed();
FfmpegResolution cv_ffmpeg_resolve(const std::string &configured);
```

`src/ffmpeg-runtime-locate.cpp`:

```cpp
#include "ffmpeg-runtime-locate.h"
#include "ffmpeg-runtime-fs.h"

#include <obs-module.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

QString cv_ffmpeg_install_root()
{
    char *raw = obs_module_config_path("ffmpeg");
    const QString root = raw ? QString::fromUtf8(raw) : QString();
    bfree(raw);
    if (!root.isEmpty())
        QDir().mkpath(root);
    return root;
}

QString cv_ffmpeg_managed_exe()
{
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) return QString();
    return QDir(cv_ffmpeg_install_root()).filePath(
        QStringLiteral("current/") + QString::fromUtf8(pin->exe_name));
}

bool cv_ffmpeg_managed_installed()
{
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) return false;
    return cvff::install_complete(
        std::filesystem::path(cv_ffmpeg_install_root().toStdWString()), pin->exe_name);
}

FfmpegResolution cv_ffmpeg_resolve(const std::string &configured)
{
    FfmpegResolveInputs in;
    in.configured = configured;
    in.managed_exe = QDir::toNativeSeparators(cv_ffmpeg_managed_exe()).toStdString();
    in.managed_complete = cv_ffmpeg_managed_installed();
#if defined(__APPLE__)
    in.macos = true;
#endif
    in.file_exists = [](const std::string &p) {
        const QFileInfo info(QString::fromStdString(p));
        return info.exists() && info.isFile();
    };
    in.find_on_path = [](const std::string &name) {
        return QStandardPaths::findExecutable(QString::fromStdString(name)).toStdString();
    };
    // The dock may hold either separator style; compare like with like.
    in.configured = QDir::toNativeSeparators(QString::fromStdString(configured).trimmed()).toStdString();
    return ffmpeg_resolve(in);
}
```

- [ ] **Step 6: Use it in `src/zoom-iso-recorder.cpp`**

Add `#include "ffmpeg-runtime-locate.h"` with the other includes. Replace the block from `if (normalized.ffmpeg_path.empty())` through the end of the "FFmpeg was not found on PATH" `return false;` with:

```cpp
    // One resolution for every caller (dock, control API, OSC): explicit
    // path, then CoreVideo's managed FFmpeg, then PATH, then Homebrew on
    // macOS. Spec 2026-10-03.
    const FfmpegResolution resolved = cv_ffmpeg_resolve(normalized.ffmpeg_path);
    if (resolved.source == FfmpegSource::None) {
        if (error) {
            *error = "FFmpeg was not found. Use Download FFmpeg in the ISO "
                     "Recorder dock, or set ffmpeg_path to a valid ffmpeg "
                     "executable.";
        }
        return false;
    }
    normalized.ffmpeg_path = resolved.path;
    const std::string requested_encoder = normalized_video_encoder(normalized.video_encoder);
    normalized.video_encoder = requested_encoder;
    const QString ffmpegProgram = QString::fromStdString(normalized.ffmpeg_path);
```

(Keep the two `requested_encoder` / `ffmpegProgram` lines exactly once; delete the originals they replace.)

In the availability probe, add VideoToolbox and log it:

```cpp
    avail.amf = ffmpeg_encoder_available(ffmpegProgram, "h264_amf", nullptr);
#if defined(__APPLE__)
    avail.videotoolbox = ffmpeg_encoder_available(ffmpegProgram, "h264_videotoolbox", nullptr);
#endif
    if (normalized.video_encoder == "auto") {
        blog(LOG_INFO,
             "[obs-zoom-plugin] ISO encoder placement: automatic "
             "(nvenc=%d qsv=%d amf=%d videotoolbox=%d, NVENC session limit %d, "
             "OBS NVENC encoders active %d)",
             avail.nvenc, avail.qsv, avail.amf, avail.videotoolbox,
             iso_nvenc_default_session_limit(), count_obs_nvenc_encoders());
```

Accept the encoder name and treat it as hardware:

```cpp
static std::string normalized_video_encoder(const std::string &encoder)
{
    if (encoder == "auto" || encoder == "h264_nvenc" || encoder == "h264_qsv" ||
        encoder == "h264_amf" || encoder == "h264_videotoolbox" || encoder == "libx264") {
        return encoder;
    }
    return "auto";
}
```

```cpp
static bool is_hardware_encoder(const std::string &encoder)
{
    return encoder == "h264_nvenc" || encoder == "h264_qsv" || encoder == "h264_amf" ||
           encoder == "h264_videotoolbox";
}
```

Add `src/ffmpeg-runtime-locate.cpp` and `src/ffmpeg-runtime-fs.cpp` to the plugin target's source list in `CMakeLists.txt`, on the line after `src/zoom-iso-recorder.cpp`.

- [ ] **Step 7: Build the plugin and run the whole suite**

Run: `cmake -S . -B build_x64 >/dev/null && cmake --build build_x64 --config Release --parallel 8 && (cd build_x64 && ctest -C Release --output-on-failure)`
Expected: build succeeds; all tests pass (85/85 with Tasks 1-3).

- [ ] **Step 8: Commit**

```bash
git add src/iso-encoder-plan.h tests/iso-encoder-plan-test.cpp src/ffmpeg-runtime-locate.h src/ffmpeg-runtime-locate.cpp src/zoom-iso-recorder.cpp CMakeLists.txt
git commit -m "ISO: resolve FFmpeg via managed/PATH/Homebrew; VideoToolbox encoder"
```

---

### Task 5: Live pin check (manual network test)

**Files:**
- Create: `tests/ffmpeg-runtime-live-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ffmpeg_runtime_pin_for_host`, `ffmpeg_download_size_ok` (Tasks 1-2).
- Produces: executable `CoreVideoFfmpegRuntimeLiveTest`, built with tests but deliberately NOT registered with `add_test`.

- [ ] **Step 1: Write the test** — `tests/ffmpeg-runtime-live-test.cpp`:

```cpp
// MANUAL, NETWORK: proves the pinned FFmpeg archive for this platform still
// downloads and still hashes to the pin. Run it whenever ffmpeg-runtime-pins.h
// changes and before a release; a drifted pin means every operator's Download
// button fails with a checksum error. Deliberately not in ctest (CI must not
// depend on gyan.dev / martin-riedl.de availability).
#include "ffmpeg-runtime-pins.h"
#include "ffmpeg-runtime-plan.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) {
        std::cout << "no FFmpeg pin for this platform; nothing to check\n";
        return 0;
    }
    std::cout << "downloading " << pin->url << "\n";
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(QString::fromUtf8(pin->url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam.get(req);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 received = 0;
    bool oversized = false;
    QObject::connect(reply, &QNetworkReply::readyRead, [&] {
        const QByteArray chunk = reply->readAll();
        received += quint64(chunk.size());
        hash.addData(chunk);
        if (!ffmpeg_download_size_ok(received, pin->size_bytes)) {
            oversized = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &app, &QCoreApplication::quit);
    app.exec();
    const QByteArray rest = reply->readAll();
    received += quint64(rest.size());
    hash.addData(rest);

    if (oversized) { std::cerr << "FAIL: archive larger than pin + 10%\n"; return 1; }
    if (reply->error() != QNetworkReply::NoError) {
        std::cerr << "FAIL: " << reply->errorString().toStdString() << "\n";
        return 1;
    }
    const std::string got = hash.result().toHex().toStdString();
    std::cout << "size " << received << " (pin " << pin->size_bytes << ")\n"
              << "sha256 " << got << "\n";
    if (received != pin->size_bytes || got != pin->sha256) {
        std::cerr << "FAIL: archive no longer matches the pin\n";
        return 1;
    }
    std::cout << "ffmpeg runtime live: pin matches upstream\n";
    return 0;
}
```

CMake (inside `if(BUILD_TESTING)`, next to the other FFmpeg runtime tests):

```cmake
    # MANUAL network check that the pinned archive still matches (not in ctest).
    add_executable(CoreVideoFfmpegRuntimeLiveTest tests/ffmpeg-runtime-live-test.cpp)
    target_include_directories(CoreVideoFfmpegRuntimeLiveTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(CoreVideoFfmpegRuntimeLiveTest PRIVATE Qt6::Core Qt6::Network)
```

- [ ] **Step 2: Build and run it once**

Run: `cmake -S . -B build_x64 >/dev/null && cmake --build build_x64 --config Release --target CoreVideoFfmpegRuntimeLiveTest && build_x64/Release/CoreVideoFfmpegRuntimeLiveTest.exe`
Expected: `size 114768076 (pin 114768076)`, then `ffmpeg runtime live: pin matches upstream`. If it fails with "TLS initialization failed", run it from a shell where `QT_PLUGIN_PATH` includes the Qt `plugins` dir from `CMAKE_PREFIX_PATH`; the test needs a TLS backend just like the plugin.

- [ ] **Step 3: Commit**

```bash
git add tests/ffmpeg-runtime-live-test.cpp CMakeLists.txt
git commit -m "FFmpeg runtime: manual live check that the pin matches upstream"
```

---

### Task 6: The installer (download, verify, extract, test-run, swap)

**Files:**
- Create: `src/ffmpeg-runtime-installer.h`, `src/ffmpeg-runtime-installer.cpp`
- Modify: `CMakeLists.txt` (plugin sources; this is a `Q_OBJECT` class, and the plugin target already has AUTOMOC)

**Interfaces:**
- Consumes: Tasks 1-4 (`ffmpeg_runtime_pin_for_host`, `kFfmpegLicenseDataFile`, `ffmpeg_archive_entry_safe`, `ffmpeg_download_size_ok`, `ffmpeg_provenance_text`, `cvff::*`, `cv_ffmpeg_install_root`, `cv_ffmpeg_managed_exe`).
- Produces: `class FfmpegRuntimeInstaller : public QObject` with `static FfmpegRuntimeInstaller &instance(); bool busy() const; void start_download(); void cancel(); bool remove(bool in_use, QString *error);` and signals `progress(qint64 received, qint64 total)`, `finished(bool ok, QString message)`, `state_changed()`.

This task has no host test. Its decisions are already covered by Tasks 1-3, and what remains is QNetwork/QProcess wiring that needs OBS. Verification is the build plus the live dock check in Task 7.

- [ ] **Step 1: Write `src/ffmpeg-runtime-installer.h`**

```cpp
#pragma once
// Fetches the pinned FFmpeg into the operator's OBS profile when they press
// Download (spec 2026-10-03). Process-wide singleton (Meyers, like
// CvUpdateChecker) so the dock can be closed and reopened mid-download.
// Never runs without an explicit operator click.
#include <QObject>
#include <QCryptographicHash>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

class FfmpegRuntimeInstaller : public QObject {
    Q_OBJECT
public:
    static FfmpegRuntimeInstaller &instance();

    bool busy() const { return m_busy; }
    // Starts a download unless one is already running (REVIEW FOCUS 4:
    // a double click must not start two). Emits finished() on every exit.
    void start_download();
    void cancel();
    // Refuses while the managed ffmpeg is recording (REVIEW FOCUS 5).
    bool remove(bool in_use, QString *error);

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString &message);
    void state_changed();

private:
    FfmpegRuntimeInstaller() = default;
    void on_ready_read();
    void on_download_finished();
    void finish(bool ok, const QString &message);
    // Runs on a worker thread: list, extract, copy, test-run, provenance,
    // swap. Returns "" on success, else the operator-facing failure line.
    static QString install_from_archive(const QString &root, const QString &archive);

    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_received = 0;
    bool m_busy = false;
    bool m_cancelled = false;
    bool m_oversized = false;
    QString m_root;
};
```

- [ ] **Step 2: Write `src/ffmpeg-runtime-installer.cpp`**

```cpp
#include "ffmpeg-runtime-installer.h"
#include "ffmpeg-runtime-fs.h"
#include "ffmpeg-runtime-locate.h"
#include "ffmpeg-runtime-plan.h"

#include <obs-module.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QThread>

static const char *kNothingChanged = " Nothing was changed.";

FfmpegRuntimeInstaller &FfmpegRuntimeInstaller::instance()
{
    static FfmpegRuntimeInstaller s;
    return s;
}

static std::filesystem::path fs_path(const QString &p)
{
    return std::filesystem::path(p.toStdWString());
}

void FfmpegRuntimeInstaller::start_download()
{
    if (m_busy) return;  // one download at a time
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!pin) {
        emit finished(false, QStringLiteral(
            "CoreVideo has no FFmpeg download for this platform. Choose an existing ffmpeg instead."));
        return;
    }
    m_root = cv_ffmpeg_install_root();
    if (m_root.isEmpty()) {
        emit finished(false, QStringLiteral("Could not locate the CoreVideo settings folder.") + kNothingChanged);
        return;
    }
    cvff::clean_leftovers(fs_path(m_root));

    // Archive + extracted exe + staged copy can coexist briefly.
    const QStorageInfo storage(m_root);
    const qint64 needed = qint64(pin->size_bytes) * 3;
    if (storage.isValid() && storage.bytesAvailable() < needed) {
        emit finished(false, QString("Not enough disk space: FFmpeg needs about %1 MB free in %2.")
                                 .arg(needed / (1024 * 1024)).arg(QDir::toNativeSeparators(m_root)) +
                                 kNothingChanged);
        return;
    }

    m_file = new QFile(QDir(m_root).filePath(QStringLiteral("download.part")), this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = m_file->errorString();
        delete m_file; m_file = nullptr;
        emit finished(false, QStringLiteral("Could not write to the CoreVideo settings folder: ") + why + kNothingChanged);
        return;
    }

    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    m_hash.reset();
    m_received = 0;
    m_cancelled = false;
    m_oversized = false;
    m_busy = true;
    emit state_changed();

    QNetworkRequest req{QUrl(QString::fromUtf8(pin->url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("CoreVideo-OBS-Plugin"));
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &FfmpegRuntimeInstaller::on_ready_read);
    connect(m_reply, &QNetworkReply::finished, this, &FfmpegRuntimeInstaller::on_download_finished);
    blog(LOG_INFO, "[obs-zoom-plugin] FFmpeg download started: %s", pin->url);
}

void FfmpegRuntimeInstaller::on_ready_read()
{
    if (!m_reply || !m_file) return;
    const QByteArray chunk = m_reply->readAll();
    m_received += chunk.size();
    m_hash.addData(chunk);
    m_file->write(chunk);
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    if (!ffmpeg_download_size_ok(quint64(m_received), pin->size_bytes)) {
        m_oversized = true;
        m_reply->abort();
        return;
    }
    emit progress(m_received, qint64(pin->size_bytes));
}

void FfmpegRuntimeInstaller::cancel()
{
    if (!m_busy || !m_reply) return;
    m_cancelled = true;
    m_reply->abort();
}

void FfmpegRuntimeInstaller::on_download_finished()
{
    QNetworkReply *reply = m_reply;
    on_ready_read();  // drain anything left
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    const auto err = reply->error();
    const QString err_text = reply->errorString();
    reply->deleteLater();
    m_reply = nullptr;
    m_file->close();
    const QString archive = m_file->fileName();
    delete m_file; m_file = nullptr;

    if (m_cancelled) { finish(false, QStringLiteral("Download cancelled.") + kNothingChanged); return; }
    if (m_oversized) { finish(false, QStringLiteral("Download failed: the file was larger than expected.") + kNothingChanged); return; }
    if (err != QNetworkReply::NoError) {
        const bool unreachable = err == QNetworkReply::HostNotFoundError ||
            err == QNetworkReply::ConnectionRefusedError || err == QNetworkReply::TimeoutError ||
            err == QNetworkReply::TemporaryNetworkFailureError ||
            err == QNetworkReply::NetworkSessionFailedError;
        finish(false, unreachable
            ? QString("Download failed: could not reach %1. Check your internet connection.").arg(pin->host) + kNothingChanged
            : QStringLiteral("Download failed: ") + err_text + "." + kNothingChanged);
        return;
    }
    if (quint64(m_received) != pin->size_bytes ||
        m_hash.result().toHex() != QByteArray(pin->sha256)) {
        finish(false, QStringLiteral("Download failed: file didn't match the expected checksum.") + kNothingChanged);
        return;
    }

    // Extraction and the test-run block for seconds; keep them off the UI thread.
    const QString root = m_root;
    QThread *worker = QThread::create([this, root, archive] {
        const QString failure = install_from_archive(root, archive);
        QMetaObject::invokeMethod(this, [this, failure] {
            finish(failure.isEmpty(), failure.isEmpty()
                ? QStringLiteral("FFmpeg %1 installed.").arg(ffmpeg_runtime_pin_for_host()->version)
                : failure);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

static bool run_tool(const QString &program, const QStringList &args, QByteArray *out, int timeout_ms)
{
    // Argument LIST, never a joined string: profile paths contain spaces and
    // non-ASCII characters (REVIEW FOCUS 3).
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForFinished(timeout_ms)) { p.kill(); p.waitForFinished(2000); return false; }
    if (out) *out = p.readAllStandardOutput();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

QString FfmpegRuntimeInstaller::install_from_archive(const QString &root, const QString &archive)
{
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    const QDir dir(root);
    const QString extract = dir.filePath(QStringLiteral("extract"));
    const QString staging = dir.filePath(QStringLiteral("staging"));
    QDir().mkpath(extract);
    QDir().mkpath(staging);
    auto fail = [&](const QString &why) {
        cvff::clean_leftovers(fs_path(root));
        return why + kNothingChanged;
    };

#if defined(_WIN32)
    const QString tar = QDir(QProcessEnvironment::systemEnvironment().value(
                                 QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows")))
                            .filePath(QStringLiteral("System32/tar.exe"));
    if (!QFileInfo::exists(tar))
        return fail(QStringLiteral("This version of Windows has no built-in tar.exe (Windows 10 1803 or newer). "
                                   "Choose an existing ffmpeg instead."));
    QByteArray listing;
    if (!run_tool(tar, {QStringLiteral("-tf"), archive}, &listing, 60000))
        return fail(QStringLiteral("Could not read the downloaded archive."));
#else
    QByteArray listing;
    if (!run_tool(QStringLiteral("/usr/bin/zipinfo"), {QStringLiteral("-1"), archive}, &listing, 60000))
        return fail(QStringLiteral("Could not read the downloaded archive."));
#endif
    bool has_exe = false;
    for (const QByteArray &line : listing.split('\n')) {
        const std::string entry = QString::fromUtf8(line).trimmed().toStdString();
        if (entry.empty()) continue;
        if (!ffmpeg_archive_entry_safe(entry))
            return fail(QStringLiteral("The downloaded archive contains an unsafe path."));
        if (entry == pin->archive_exe) has_exe = true;
    }
    if (!has_exe)
        return fail(QStringLiteral("The downloaded archive does not contain ffmpeg."));

#if defined(_WIN32)
    if (!run_tool(tar, {QStringLiteral("-xf"), archive, QStringLiteral("-C"), extract,
                        QString::fromUtf8(pin->archive_exe)}, nullptr, 300000))
        return fail(QStringLiteral("Could not unpack FFmpeg."));
#else
    if (!run_tool(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-x"), QStringLiteral("-k"), archive, extract},
                  nullptr, 300000))
        return fail(QStringLiteral("Could not unpack FFmpeg."));
#endif

    const QString exe = QDir(staging).filePath(QString::fromUtf8(pin->exe_name));
    if (!QFile::copy(QDir(extract).filePath(QString::fromUtf8(pin->archive_exe)), exe))
        return fail(QStringLiteral("Could not stage FFmpeg."));
    QFile::setPermissions(exe, QFile::permissions(exe) | QFileDevice::ExeOwner |
                                   QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);

    char *license = obs_module_file(kFfmpegLicenseDataFile);
    const bool license_ok = license &&
        QFile::copy(QString::fromUtf8(license), QDir(staging).filePath(QStringLiteral("LICENSE.txt")));
    bfree(license);
    if (!license_ok)
        return fail(QStringLiteral("CoreVideo's FFmpeg license file is missing; reinstall CoreVideo."));

    QByteArray version;
    if (!run_tool(exe, {QStringLiteral("-hide_banner"), QStringLiteral("-version")}, &version, 30000) ||
        !version.startsWith("ffmpeg version"))
        return fail(QStringLiteral("The downloaded FFmpeg did not start on this computer."));

    QFile prov(QDir(staging).filePath(QStringLiteral("provenance.txt")));
    if (!prov.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write provenance.txt."));
    prov.write(QByteArray::fromStdString(ffmpeg_provenance_text(
        *pin, QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString())));
    prov.close();

    QDir(extract).removeRecursively();
    QFile::remove(archive);
    std::string swap_error;
    if (!cvff::swap_in_staging(fs_path(root), &swap_error))
        return fail(QString::fromStdString(swap_error));
    return QString();
}

void FfmpegRuntimeInstaller::finish(bool ok, const QString &message)
{
    if (!ok && !m_root.isEmpty())
        cvff::clean_leftovers(fs_path(m_root));
    m_busy = false;
    blog(ok ? LOG_INFO : LOG_WARNING, "[obs-zoom-plugin] FFmpeg download: %s",
         message.toUtf8().constData());
    emit state_changed();
    emit finished(ok, message);
}

bool FfmpegRuntimeInstaller::remove(bool in_use, QString *error)
{
    if (m_busy) {
        if (error) *error = QStringLiteral("FFmpeg is still downloading.");
        return false;
    }
    std::string why;
    const bool ok = cvff::remove_install(fs_path(cv_ffmpeg_install_root()), in_use, &why);
    if (!ok && error) *error = QString::fromStdString(why);
    emit state_changed();
    return ok;
}
```

Add `src/ffmpeg-runtime-installer.cpp` to the plugin source list after `src/ffmpeg-runtime-locate.cpp`.

- [ ] **Step 3: Build and run the suite**

Run: `cmake -S . -B build_x64 >/dev/null && cmake --build build_x64 --config Release --parallel 8 && (cd build_x64 && ctest -C Release --output-on-failure)`
Expected: build clean; all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/ffmpeg-runtime-installer.h src/ffmpeg-runtime-installer.cpp CMakeLists.txt
git commit -m "FFmpeg runtime: installer (download, verify, unpack, test-run, swap)"
```

---

### Task 7: ISO Recorder dock (status, Download/Cancel/Remove, first use, macOS picker)

**Files:**
- Modify: `src/zoom-iso-panel.h` (members and slots)
- Modify: `src/zoom-iso-panel.cpp`:
  - FFmpeg row ~lines 322-338;
  - encoder menu ~340-352;
  - `browse_ffmpeg` ~492;
  - `test_ffmpeg` ~502;
  - `start_recording` ~522;
  - `refresh_status` ~661;
  - `encoder_guidance_text` ~223;
  - `is_hardware_encoder` ~252;
  - remove `ffmpeg_exists` ~58.

**Interfaces:**
- Consumes: `FfmpegRuntimeInstaller` (Task 6); `cv_ffmpeg_resolve`, `cv_ffmpeg_managed_exe`, `cv_ffmpeg_managed_installed` (Task 4); `ffmpeg_runtime_pin_for_host` (Task 1).
- Produces: none (leaf UI).

- [ ] **Step 1: Add members to `src/zoom-iso-panel.h`**

Add the forward declaration `class QProgressBar;` near the others. In the private section, add:

```cpp
    void download_ffmpeg();
    void remove_ffmpeg();
    void refresh_ffmpeg_status();
    // Returns true when an FFmpeg is resolvable; otherwise offers Download /
    // Choose existing / Cancel and returns false (spec 2026-10-03 first use).
    bool ensure_ffmpeg_for_start();
    bool recording_active() const;

    QLabel *m_ffmpeg_status = nullptr;
    QPushButton *m_ffmpeg_download_btn = nullptr;
    QPushButton *m_ffmpeg_remove_btn = nullptr;
    QProgressBar *m_ffmpeg_progress = nullptr;
```

- [ ] **Step 2: Build the FFmpeg row** in the constructor, replacing `ffmpeg_row->addWidget(m_test_btn); config_layout->addLayout(ffmpeg_row);` with:

```cpp
    ffmpeg_row->addWidget(m_test_btn);
    config_layout->addLayout(ffmpeg_row);

    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    auto *managed_row = new QHBoxLayout;
    managed_row->setSpacing(6);
    m_ffmpeg_status = new QLabel(config_group);
    m_ffmpeg_status->setWordWrap(true);
    m_ffmpeg_download_btn = new QPushButton(pin
        ? QString("Download FFmpeg (~%1 MB)").arg(pin->size_bytes / (1000 * 1000))
        : QStringLiteral("Download FFmpeg"), config_group);
    m_ffmpeg_download_btn->setVisible(pin != nullptr);
    m_ffmpeg_remove_btn = new QPushButton("Remove", config_group);
    m_ffmpeg_progress = new QProgressBar(config_group);
    m_ffmpeg_progress->setVisible(false);
    m_ffmpeg_progress->setTextVisible(true);
    managed_row->addWidget(m_ffmpeg_status, 1);
    managed_row->addWidget(m_ffmpeg_progress, 1);
    managed_row->addWidget(m_ffmpeg_download_btn);
    managed_row->addWidget(m_ffmpeg_remove_btn);
    config_layout->addLayout(managed_row);

    connect(m_ffmpeg_download_btn, &QPushButton::clicked, this, &ZoomIsoPanel::download_ffmpeg);
    connect(m_ffmpeg_remove_btn, &QPushButton::clicked, this, &ZoomIsoPanel::remove_ffmpeg);
    connect(m_ffmpeg_path, &QLineEdit::textChanged, this, [this] { refresh_ffmpeg_status(); });
    auto &installer = FfmpegRuntimeInstaller::instance();
    connect(&installer, &FfmpegRuntimeInstaller::state_changed, this,
            [this] { if (!m_shutting_down) refresh_ffmpeg_status(); });
    connect(&installer, &FfmpegRuntimeInstaller::progress, this,
            [this](qint64 received, qint64 total) {
                if (m_shutting_down) return;
                m_ffmpeg_progress->setRange(0, 1000);
                m_ffmpeg_progress->setValue(total > 0 ? int(received * 1000 / total) : 0);
                m_ffmpeg_progress->setFormat(QString("Downloading FFmpeg... %1 / %2 MB")
                    .arg(received / (1000 * 1000)).arg(total / (1000 * 1000)));
            });
    connect(&installer, &FfmpegRuntimeInstaller::finished, this,
            [this](bool ok, const QString &message) {
                if (m_shutting_down) return;
                if (ok) {
                    m_ffmpeg_path->setText(QDir::toNativeSeparators(cv_ffmpeg_managed_exe()));
                    persist_settings();
                    set_error(QString());
                    test_ffmpeg();
                } else {
                    set_error(message);
                }
                refresh_ffmpeg_status();
            });
```

Add includes at the top of `zoom-iso-panel.cpp`: `#include "ffmpeg-runtime-installer.h"`, `#include "ffmpeg-runtime-locate.h"`, `#include <QProgressBar>`. At the end of the constructor, after the existing initial refresh calls, add `refresh_ffmpeg_status();`.

- [ ] **Step 3: Make the encoder menu platform-correct**

Replace the four hardware `addItem` lines with:

```cpp
#if defined(__APPLE__)
    m_video_encoder->addItem("Apple VideoToolbox - H.264", "h264_videotoolbox");
#else
    m_video_encoder->addItem("NVIDIA NVENC - H.264", "h264_nvenc");
    m_video_encoder->addItem("Intel Quick Sync - H.264", "h264_qsv");
    m_video_encoder->addItem("AMD AMF - H.264", "h264_amf");
#endif
```

In `encoder_guidance_text`, before the final `return`, add:

```cpp
    if (encoder == QStringLiteral("h264_videotoolbox")) {
        return QStringLiteral(
            "Apple VideoToolbox uses the Mac's media engine and keeps CPU load low. "
            "If FFmpeg reports an encoder failure, the feed falls back to CPU x264.");
    }
```

In the `auto` guidance branch, add a macOS-specific sentence using `#if defined(__APPLE__)`: return `"Automatic uses Apple VideoToolbox when this FFmpeg build has it, then CPU x264. The Encoder column shows each feed's placement."` on macOS, and keep the existing Windows text in `#else`. Extend `is_hardware_encoder` with `|| encoder == QStringLiteral("h264_videotoolbox")`.

- [ ] **Step 4: Fix Browse and Test**

`browse_ffmpeg`:

```cpp
void ZoomIsoPanel::browse_ffmpeg()
{
#if defined(_WIN32)
    const QString filter = QStringLiteral("FFmpeg (ffmpeg.exe);;All files (*)");
#else
    const QString filter = QStringLiteral("FFmpeg (ffmpeg);;All files (*)");
#endif
    const QString path = QFileDialog::getOpenFileName(
        this, "Select ffmpeg executable", QString(), filter);
    if (!path.isEmpty()) {
        m_ffmpeg_path->setText(QDir::toNativeSeparators(path));
        persist_settings();
    }
}
```

`test_ffmpeg`: replace the `ffmpeg_exists` check and the `ffmpeg_has_encoder(m_ffmpeg_path->text(), …)` argument so they use the resolved path:

```cpp
void ZoomIsoPanel::test_ffmpeg()
{
    persist_settings();
    const FfmpegResolution r = cv_ffmpeg_resolve(m_ffmpeg_path->text().toStdString());
    if (r.source == FfmpegSource::None) {
        set_error("FFmpeg was not found. Use Download FFmpeg, or Browse to an existing ffmpeg.");
        return;
    }
    const QString program = QString::fromStdString(r.path);
    const QString encoder = m_video_encoder->currentData().toString();
    if (encoder != QStringLiteral("auto") && !ffmpeg_has_encoder(program, encoder)) {
        set_error(QString("FFmpeg was found, but encoder '%1' is not available in this FFmpeg build.")
            .arg(encoder));
        return;
    }
    set_error(QString());
    QMessageBox::information(this, "FFmpeg",
        QString("FFmpeg was found at %1 and encoder '%2' is available.")
            .arg(QDir::toNativeSeparators(program), encoder));
}
```

Delete the now-unused `static bool ffmpeg_exists(...)`.

- [ ] **Step 5: Status, download, remove, first use**

Add these member functions:

```cpp
bool ZoomIsoPanel::recording_active() const
{
    // Same two facts refresh_status() uses for the Start button.
    auto &recorder = ZoomIsoRecorder::instance();
    return recorder.active() || recorder.status_overview().value("finishing").toBool();
}

void ZoomIsoPanel::refresh_ffmpeg_status()
{
    const auto &installer = FfmpegRuntimeInstaller::instance();
    const bool busy = installer.busy();
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    const bool managed = cv_ffmpeg_managed_installed();
    const FfmpegResolution r = cv_ffmpeg_resolve(m_ffmpeg_path->text().toStdString());

    QString text;
    QString color;
    switch (r.source) {
    case FfmpegSource::Managed:
        text = QString("Managed FFmpeg %1").arg(pin ? pin->version : "");
        break;
    case FfmpegSource::Explicit:
    case FfmpegSource::Path:
    case FfmpegSource::Homebrew:
        text = QString("Using ffmpeg at %1").arg(QDir::toNativeSeparators(QString::fromStdString(r.path)));
        break;
    case FfmpegSource::None:
        text = QStringLiteral("FFmpeg not found");
        color = QStringLiteral("#e0a030");
        break;
    }
    if (r.configured_missing && r.source != FfmpegSource::None)
        text = QStringLiteral("Configured FFmpeg not found; ") + text.left(1).toLower() + text.mid(1);
    m_ffmpeg_status->setText(text);
    m_ffmpeg_status->setStyleSheet(color.isEmpty() ? QString() : QString("color: %1;").arg(color));

    m_ffmpeg_progress->setVisible(busy);
    m_ffmpeg_status->setVisible(!busy);
    m_ffmpeg_download_btn->setVisible(pin && (busy || !managed));
    m_ffmpeg_download_btn->setText(busy ? QStringLiteral("Cancel")
        : (pin ? QString("Download FFmpeg (~%1 MB)").arg(pin->size_bytes / (1000 * 1000)) : QString()));
    // REVIEW FOCUS 5: never offer Remove while a recording could be using it.
    m_ffmpeg_remove_btn->setVisible(managed && !busy);
    m_ffmpeg_remove_btn->setEnabled(!recording_active());
    m_ffmpeg_remove_btn->setToolTip(recording_active()
        ? QStringLiteral("Stop ISO recording before removing FFmpeg.") : QString());
}

void ZoomIsoPanel::download_ffmpeg()
{
    auto &installer = FfmpegRuntimeInstaller::instance();
    if (installer.busy()) {
        installer.cancel();
        return;
    }
    set_error(QString());
    installer.start_download();
    refresh_ffmpeg_status();
}

void ZoomIsoPanel::remove_ffmpeg()
{
    QString error;
    const QString managed = QDir::toNativeSeparators(cv_ffmpeg_managed_exe());
    if (!FfmpegRuntimeInstaller::instance().remove(recording_active(), &error)) {
        set_error(error);
        return;
    }
    if (QDir::toNativeSeparators(m_ffmpeg_path->text().trimmed()) == managed) {
        m_ffmpeg_path->setText(QStringLiteral("ffmpeg"));
        persist_settings();
    }
    set_error(QString());
    refresh_ffmpeg_status();
}

bool ZoomIsoPanel::ensure_ffmpeg_for_start()
{
    // REVIEW FOCUS 4: a download in flight is not "missing".
    if (FfmpegRuntimeInstaller::instance().busy()) {
        set_error("FFmpeg is still downloading. Start ISO recording when it finishes.");
        return false;
    }
    if (cv_ffmpeg_resolve(m_ffmpeg_path->text().toStdString()).source != FfmpegSource::None)
        return true;
    const FfmpegRuntimePin *pin = ffmpeg_runtime_pin_for_host();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("FFmpeg needed");
    QPushButton *download = nullptr;
    if (pin) {
        box.setText(QString("ISO recording needs FFmpeg. Download it now (~%1 MB from %2)?")
                        .arg(pin->size_bytes / (1000 * 1000)).arg(pin->host));
        download = box.addButton("Download", QMessageBox::AcceptRole);
    } else {
        box.setText("ISO recording needs FFmpeg. Choose an existing ffmpeg executable.");
    }
    QPushButton *choose = box.addButton("Choose existing...", QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (download && box.clickedButton() == download)
        download_ffmpeg();
    else if (box.clickedButton() == choose)
        browse_ffmpeg();
    return false;  // the operator presses Start again once FFmpeg is ready
}
```

In `start_recording()`, after the empty-selection check and before `persist_settings();`, add:

```cpp
    if (!ensure_ffmpeg_for_start())
        return;
```

At the end of `refresh_status()`, add `refresh_ffmpeg_status();` so Remove's enabled state tracks recording.

- [ ] **Step 6: Build and run the suite**

Run: `cmake --build build_x64 --config Release --parallel 8 && (cd build_x64 && ctest -C Release --output-on-failure)`
Expected: build clean; all tests pass.

- [ ] **Step 7: Live check in OBS on Windows** (install the built pair as described in CLAUDE.md, "Installing to Program Files")
  1. Rename any `ffmpeg` on PATH out of the way. The status should read amber "FFmpeg not found".
  2. Press Start ISO with a feed selected. The first-use dialog appears and names `github.com (gyan.dev builds)`.
  3. Choose Download. A progress bar appears; press Cancel. The result should be "Download cancelled. Nothing was changed.", and no `download.part` is left in `%APPDATA%\obs-studio\plugin_config\obs-zoom-plugin\ffmpeg\`.
  4. Download again and let it finish. The field fills with the managed path, the Test dialog shows it, and the status reads "Managed FFmpeg 9.0.2".
  5. Record a short two-feed ISO. Both MP4s play.
  6. During a recording, Remove is disabled with its tooltip. Stop the recording, press Remove, and the status returns to "FFmpeg not found".

- [ ] **Step 8: Commit**

```bash
git add src/zoom-iso-panel.h src/zoom-iso-panel.cpp
git commit -m "ISO dock: FFmpeg status, Download/Cancel/Remove, first-use prompt, macOS picker"
```

---

### Task 8: Docs, notices, changelog

**Files:**
- Modify: `docs/policies/privacy-policy.md` (new §4.4; renumber "No Other Third-Party Services" to §4.5; extend the §3 network sentence at line 49)
- Create: `THIRD-PARTY-NOTICES.md`
- Modify: `docs/OPERATOR_QUICKSTART.md` (ISO Recording section, ~line 153)
- Modify: `CLAUDE.md` (Invariants list)
- Modify: `CHANGELOG.md` (`[Unreleased]`)

**Interfaces:** none.

- [ ] **Step 1: Privacy policy.** At line 49, change the sentence ending "…and GitHub only for the opt-out-able update check described in §4.3." to "…GitHub only for the opt-out-able update check described in §4.3, and the FFmpeg download hosts described in §4.4 only when the operator clicks Download FFmpeg." Insert before the current §4.4:

```markdown
### 4.4 FFmpeg Download (operator-initiated)
ISO recording runs an FFmpeg program. When the operator clicks **Download FFmpeg** in the ISO Recorder dock (or chooses Download when ISO recording asks for it), CoreVideo makes one anonymous HTTPS download of a pinned FFmpeg build: from `github.com` (gyan.dev builds) on Windows, or from `ffmpeg.martin-riedl.de` on macOS. The request carries no meeting data, credentials, telemetry or CoreVideo-specific identifiers, only what those hosts log for any anonymous download (e.g. IP address). CoreVideo never downloads FFmpeg without that click, never at startup or install time, and verifies the file against a SHA-256 hash built into CoreVideo before using it. The binary is stored in the operator's OBS settings folder and can be removed with **Remove** in the same dock.
```

Then renumber the old "### 4.4 No Other Third-Party Services" heading to "### 4.5".

- [ ] **Step 2: `THIRD-PARTY-NOTICES.md`**

```markdown
# Third-party notices

## FFmpeg (downloaded on request, not redistributed)

ISO recording uses FFmpeg (https://ffmpeg.org), licensed under the GNU General
Public License v3 for the builds CoreVideo uses. CoreVideo does **not** ship
FFmpeg. When the operator clicks *Download FFmpeg*, CoreVideo downloads a
pinned build directly from its upstream publisher and verifies its SHA-256:

| Platform | Publisher | Build |
|---|---|---|
| Windows x64 | gyan.dev (https://www.gyan.dev/ffmpeg/builds/) | 9.0.2 GPL essentials |
| macOS Apple Silicon | Martin Riedl (https://ffmpeg.martin-riedl.de) | 9.0.2 GPL |

The GPLv3 text is installed next to the binary as `LICENSE.txt`, together with
`provenance.txt` recording the exact source URL and hash. FFmpeg source code
is available from https://ffmpeg.org/download.html and from each publisher.
```

- [ ] **Step 3: Operator quickstart.** In `## ISO Recording`, insert a new step 2 and renumber the following steps:

```markdown
2. If the status line says **FFmpeg not found**, click **Download FFmpeg**.
   CoreVideo fetches a verified FFmpeg build (about 115 MB on Windows, 28 MB on
   Mac) into your OBS settings folder; no admin rights are needed. To use your
   own FFmpeg instead, click **Browse**. On a Mac, an FFmpeg installed with
   Homebrew is found automatically.
```

In the encoder step, add the line: "   - On a Mac, `h264_videotoolbox` (Apple VideoToolbox) is the hardware encoder."

- [ ] **Step 4: CLAUDE.md.** Add an Invariants bullet after the "ISO ffmpeg feed" bullet:

```markdown
- **Managed FFmpeg is pinned, user-initiated, per-user** (`src/ffmpeg-runtime-*`,
  spec `docs/superpowers/specs/2026-10-03-ffmpeg-runtime-download-design.md`):
  ISO recording runs an external `ffmpeg` that no package ships. **Download
  FFmpeg** fetches the pin for the host (`ffmpeg-runtime-pins.h`: gyan.dev
  9.0.2 essentials on Windows, Martin Riedl 9.0.2 on Apple Silicon, both GPLv3)
  ONLY on an operator click, verifies the compiled-in SHA-256 (never an
  upstream `.sha256`), unpacks it with the OS tool (`tar.exe` / `ditto`) after
  rejecting unsafe entries, test-runs `-version`, and swaps it into
  `obs_module_config_path("ffmpeg")/current` by rename, so a failed or
  interrupted update never loses the old install (`clean_leftovers` restores
  `previous/` after a mid-swap crash). The macOS zip has no license, so
  `data/ffmpeg/LICENSE-GPLv3.txt` is installed beside every managed binary.
  Resolution, shared by the dock, control API and OSC through
  `ZoomIsoRecorder::start()`: explicit path, then managed, then PATH, then
  Homebrew (macOS; OBS from the Dock has no shell PATH). To move FFmpeg,
  edit BOTH pins and run `CoreVideoFfmpegRuntimeLiveTest` on each platform.
```

- [ ] **Step 5: CHANGELOG.** Under `## [Unreleased]`:

```markdown
### Added
- **Download FFmpeg** in the ISO Recorder dock: one click fetches a verified
  FFmpeg 9.0.2 build (Windows and Apple Silicon) into your OBS settings folder,
  with no admin rights and no separate install. ISO recording offers it the
  first time it needs FFmpeg. Nothing is downloaded without that click.

### Fixed
- macOS: the FFmpeg picker no longer asks for `ffmpeg.exe`, Homebrew's FFmpeg
  is found even when OBS is launched from the Dock, and ISO recording offers
  Apple VideoToolbox in place of the Windows-only NVIDIA/Intel/AMD encoders.
```

- [ ] **Step 6: Run the suite once more and commit**

Run: `(cd build_x64 && ctest -C Release --output-on-failure)`
Expected: all tests pass.

```bash
git add docs/policies/privacy-policy.md THIRD-PARTY-NOTICES.md docs/OPERATOR_QUICKSTART.md CLAUDE.md CHANGELOG.md
git commit -m "Docs: managed FFmpeg download (privacy, notices, quickstart, CLAUDE.md)"
```

---

## Acceptance before release (not a coding task)

- Windows: Task 7 Step 7, passed in real OBS.
- Owner's Mac (the only Mac; Apple signing happens there):
  1. Build and install with `scripts/make-macos-bundle.sh --build-dir <dir> --link-sdk --install`.
  2. With no ffmpeg on the machine, press Download. The status should read "Managed FFmpeg 9.0.2".
  3. Gatekeeper must not block it. It is downloaded by the app, not a browser, so there is no quarantine flag, and the binary is signed.
  4. The encoder menu shows Apple VideoToolbox, and a VideoToolbox ISO records.
  5. With Homebrew ffmpeg installed and the managed copy removed, the status reads "Using ffmpeg at /opt/homebrew/bin/ffmpeg".
- `CoreVideoFfmpegRuntimeLiveTest` passes on both platforms.
