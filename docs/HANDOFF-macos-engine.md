# macOS port — handoff to a Mac session

Phase 1 (real plugin builds in CI) and a Phase-2 engine **scaffold** are done on the
`mac-port` branch (DRAFT PR #148). CI is green on Windows, Linux, Companion, and macos-14
(arm64): the real plugin bundle + OAuth helper build and ctest passes 17/17; the engine
scaffold compiles and links against the real ZoomSDK.framework. **None of it has ever been
run.** You have a real Apple Silicon Mac, so your job is the part CI cannot do: make it
actually work and verify it at runtime.

## Start here
- Work on your own branch off `mac-port` (e.g. `git checkout mac-port && git pull &&
  git checkout -b mac-port-mac`). Never push to `main`. Do **not** modify the Windows job
  steps in `.github/workflows`. **Never commit any Zoom SDK file.** Never publish the draft
  release "SDK assets (private storage - DO NOT PUBLISH)".
- Read PR #148's body (full scoping). Then read, in the repo: `src/engine-ipc.h` (the IPC +
  SHM contract both sides must honor), `engine/src/main.cpp` + `engine/src/engine-video.cpp`
  / `engine-share.cpp` / `engine-audio.cpp` (the **working** Windows/Linux engine — the
  behavior you must reproduce), and `engine/src/main-macos.mm` (the current loud-failing
  scaffold you're replacing).

## The key fact (already verified)
The macOS Zoom Meeting SDK v7.1.5 is **Objective-C only**. It shares zero API with the
engine: no `zoom_sdk.h`, no `ZOOMSDK::` C++ namespace, no `*_interface.h`. The engine is
written entirely against the C++ surface (`ZOOMSDK::IAuthService`/`IMeetingService`,
`SDKAuth(AuthContext)`, `InitSDK(InitParam)`, raw-data delegates `IZoomSDKRendererDelegate`
/ `IZoomSDKAudioRawDataDelegate`). The mac framework instead exposes ObjC classes + delegate
protocols: `ZoomSDKAuthService`, `ZoomSDKMeetingService`, `ZoomSDKRawDataVideoSourceController`
(`ZoomSDKYUVProcessDataI420`, `ZoomSDKRenderer`), `ZoomSDKRawDataAudioSourceController`,
`ZoomSDKRawDataController`. So the macOS engine is a **full Objective-C++ rewrite** of
`main-macos.mm` — not an `#ifdef` port. Study the headers under
`ZoomSDK.framework/Headers/` (`ZoomSDKAuthService.h`, `ZoomSDKMeetingService.h`,
`ZoomSDKRawDataVideoSourceController.h`, `ZoomSDKRawDataAudioSourceController.h`,
`ZoomSDKRenderer.h`, `ZoomSDKSettingService.h`) before writing code.

## What to build (the engine rewrite, keeping the exact IPC wire protocol)
Reproduce `main.cpp`'s behavior against the ObjC API:
1. **init**: `ZoomSDKAuthService` init + auth with jwt or public_app_key (from the `init` IPC
   msg) → emit `auth_ok` / `auth_fail`, mapping ObjC auth results to the same codes/names the
   plugin already parses in `src/zoom-engine-client.cpp`.
2. **join**: `ZoomSDKMeetingService` join-without-login with meeting number / passcode /
   display name / ZAK / on-behalf / app-privilege tokens → emit `joined` / `left` / `error`
   with the same JSON.
3. **roster + active speaker**: via the ObjC participants controller/delegates → emit
   `participants` and `active_speaker` JSON identical to `main.cpp`.
4. **raw media**: start raw recording, then subscribe I420 video (per participant + spotlight
   + screenshare) via `ZoomSDKRawDataVideoSourceController`/`ZoomSDKRenderer` and audio via
   `ZoomSDKRawDataAudioSourceController`, writing frames into the SHM regions exactly as the
   engine side of `src/engine-ipc.h` expects (`ShmFrameHeader`/`ShmAudioHeader`, `shm_gen`
   generation, the `frame`/`audio` IPC events). **The plugin read side is already built and
   unchanged — match its contract byte-for-byte.**

Keep the loud-fail discipline: any path you haven't implemented emits a clear error over IPC
and never silently pretends. **Do not fake frames.**

## Runtime verification (this is why you're on a Mac — do all of it)
- Build locally against OBS.app. Do **not** follow CI's `brew install cmake qt@6`: the dev Mac
  has no Homebrew and cannot get one (`/opt` is root-owned, no sudo). The working toolchain is
  cmake/ninja/ctest via `pip --user`, `gh` from a release tarball, and Qt **6.8.3** via
  `aqtinstall` — 6.8 because OBS.app bundles Qt 6.8 (its Info.plist says 6.8 even though the
  shipped Qt is newer, so match at load time, not by reading the plist). macOS 26's SDK also
  dropped `AGL.framework` while Qt's `FindWrapOpenGL` still links it unconditionally; pass
  `-DWrapOpenGL_AGL=$(xcrun --show-sdk-path)/System/Library/Frameworks/OpenGL.framework`.
  That is an environment issue, so keep it a configure flag and never commit it.
- Assemble and install with `scripts/make-macos-bundle.sh --build-dir build --install`.
  Never hand-assemble the bundle — that is how the Qt double-load and missing-TLS-backend
  bugs kept coming back. Add `--link-sdk` for fast local iteration (symlinks the 612 MB Zoom
  SDK instead of copying it; not distributable).
- Load in OBS.app: confirm the source/dock/dialogs appear and the plugin logs cleanly.
- **Real join**: join an actual Zoom meeting; verify the roster populates, live video renders
  in the OBS source, audio works, and active-speaker + spotlight + screenshare work. This is
  the whole point — the historical blocker is "joins but the plugin never gets frames"; prove
  the mac path end-to-end.
- **Keychain** (Phase 3, already compiles): verify SecItem store/read/rotate under OBS's
  process context — tokens persist across restarts and never land in `global.ini` plaintext;
  corrupt/missing item → re-auth, never silent stale.
- **OAuth**: verify `corevideo://` URL-scheme registration and the callback round-trip.

## Packaging / signing / notarization (needs your Mac + the owner's Apple Developer ID)
- Bundle `ZoomSDK.framework` + its ~30 sibling frameworks/dylibs with correct
  `@rpath`/`install_name` rewrites so the engine finds them at runtime.
- Decide arm64-only vs universal (`arm64;x86_64`). `codesign` with Developer ID, then
  notarize + staple. Produce an installable artifact (pkg or drag-install zip), not the CI
  compile-only zip.

## Gotchas (already known)
- **The Zoom SDK runtime must be inside the engine's app bundle.** `ZoomSDK.framework` is not
  self-contained: at auth time it loads sibling *bundles* (`ssb_sdk`, `zNet`, `zPTUIEx`, …)
  which it locates through the **main bundle's `Contents/Frameworks`** — not through rpath and
  not relative to the framework itself. Miss them and the failure lies to you: `initSDK`
  returns Success, `getAuthService` returns a live object, then `sdkAuth` returns
  `ZoomSDKError_Failed(1)` *synchronously* and no delegate ever fires. The engine therefore
  ships as `ZoomObsEngine.app` with the SDK in `Contents/Frameworks`;
  `preflight_sdk_runtime()` in `main-macos.mm` now reports this as `sdk_runtime_missing`
  instead of leaving it to be re-diagnosed. **Verified 2026-08-01**: with the bundle correct,
  a well-formed bogus-signature JWT returns `auth_fail code 7 AUTHRET_JWTTOKENWRONG` — a real
  server verdict, so the ObjC++ auth path and the `AUTHRET_*` mapping both work end to end.
- macOS POSIX shm names are limited to 31 chars including the leading `/` (`PSHMNAMLEN`),
  verified empirically. The cause is **not** a "64-char source UUID" as earlier drafts of this
  doc claimed: `make_source_uuid()` emits only ~22 chars, and the `ZoomObsPlugin_` prefix eats
  14 of the 31 by itself, so shortening the UUID could never have fixed it. Fixed in `c009250`
  by hashing the whole name inside `shm_region_create`/`shm_region_open_read` on the Apple
  branch only, so both sides agree and Windows/Linux wire behavior is untouched.
- `-undefined dynamic_lookup` must be passed via CMake `SHELL:` (already fixed) — don't
  regress it.
- The mac engine links `-F<sdk_dir> -framework ZoomSDK`; `-F` must be on both compile and
  link lines.
- The SDK is fetched in CI from the draft-release asset via `gh api` with
  `permissions: contents: write` (draft releases are invisible to a read-only token). Keep
  that; keep the fork fallback.

## Hygiene
Loop: edit → build/run locally to verify → commit → push → `gh run watch` → fix. Batch fixes.
Keep Windows/Linux/Companion green (you edit shared code: `engine-ipc.h`,
`zoom-engine-client.cpp`). Update CLAUDE.md/README in the same change as substantive work.
When the engine joins a real meeting and delivers frames, and packaging is signed + notarized,
flip the PR out of draft with an honest report of what's runtime-verified vs. still open.
