# CoreVideo Release Checklist

Use this checklist before publishing a GitHub Release or installer.

The Windows release workflow builds and validates the installer and ZIP using
the restricted Zoom Meeting SDK from the private SDK draft. It stages the
packages on a release draft. A maintainer can also build the same package layout
locally on Windows.

Build the macOS package on the maintainer's Mac with Developer ID signing and
notarization. Attach the package and checksum to the same draft before
publishing a dual-platform release. Keep the SDK draft private.

## Build Inputs

- `main` is up to date and all intended commits are pushed.
- The Zoom SDK runtime is unpacked on the build machine at
  `third_party/zoom-sdk` (or pass `-ZoomSdkDir`).
- Windows build embeds the production Public Client ID:
  `y6sIWSwiTZe1JygMx4C9EQ`.
- Windows build embeds the same value as the Meeting SDK public app key.
- No OAuth or Meeting SDK client secret is embedded in the plugin binary.
- FFmpeg runtime strategy is documented for ISO recording.

## Local Build and Validation

Run the unit tests, then build and package:

```powershell
ctest --test-dir build-rel -C Release --output-on-failure
git diff --check
.\scripts\release-local.ps1 -Version vX.Y.Z -BuildPath build-rel -Configuration Release
```

`release-local.ps1` builds, installs into a staging folder, runs
`Test-CoreVideoPackage.ps1 -FullRuntime`, then writes the ZIP, the NSIS
installer (when `makensis` is on PATH or installed in the default location),
a `.sha256` beside each, and the install-layout manifest into `dist/`. Without
`-Upload` it publishes nothing.

The build directory must already be configured. The first time, add
`-Configure` together with the CMake paths for that machine (`-Generator`,
`-CMakePrefixPath`, `-LibObsDir`, `-ObsFrontendApiDir`, `-QtRootDir`). If
`C:\ffmpeg` holds a shared FFmpeg dev tree, hardware I420->NV12 conversion is
enabled and those DLLs are bundled automatically; `-DisableFfmpegHwAccel`
forces a CPU-only build.

Confirm `scripts/Test-CoreVideoPackage.ps1` validates:

- OBS plugin DLL.
- OAuth callback helper.
- Qt runtime and TLS plugins.
- Locale data.
- The Tiles wall's effect (`data/.../effects/corevideo-tiles.effect`).
- Zoom runtime files for full releases.
- Embedded OAuth public client ID.
- Embedded Meeting SDK public app key.
- FFmpeg runtime consistency when FFmpeg DLLs are present.
- A deterministic install-layout manifest:
  `dist/CoreVideo-Windows-x64-<version>.manifest.json`.

## OBS Smoke Tests

With OBS closed, install the package or installer. Then open OBS and verify:

- Zoom Control dock opens.
- Zoom Output Manager dock opens, closes, and reopens.
- Zoom Diagnostics dock opens, closes, and reopens.
- Zoom ISO Recorder opens.
- A CoreVideo participant source can be created.
- A CoreVideo Active Speaker source can be created.
- A CoreVideo screen-share assignment is available.
- Closing OBS from the Exit menu does not crash.

Use the smoke audit script where possible:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -AuditOnly -VerifyCoreVideoPlugin -ExpectShutdown
```

For a deeper local smoke pass that instantiates the actual CoreVideo source
kinds and lays out an 8-slot scene:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -VerifyCoreVideoPlugin -CreateCoreVideoInputs `
  -ParticipantCount 8
```

After manually opening each CoreVideo dock from OBS **Tools**, validate the OBS
log contains both registration and show markers:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -LogOnly -ExpectDockShow -ExpectShutdown `
  -ObsLogPath "$env:APPDATA\obs-studio\logs\YYYY-MM-DD HH-MM-SS.txt" `
  -ExpectedDockId ZoomControlDock,ZoomOutputManagerDock,ZoomDiagnosticsDock,ZoomIsoRecorderDock
```

## Production Flow Test

Before release, run at least one meeting test:

1. Sign in with Zoom.
2. Join a meeting.
3. Assign two or more participant sources.
4. Request 1080p and confirm Diagnostics reports observed quality.
5. Assign the active speaker source and test hold/sensitivity/manual override.
6. Assign screen share when a share is active.
7. Start ISO recording for assigned outputs.
8. Export a Diagnostics support bundle.
9. Leave the meeting.
10. Close OBS and confirm no crash.

For 8-feed testing, run:

```powershell
.\scripts\Measure-CoreVideoLoad.ps1 -DurationSeconds 1800 -SampleSeconds 5 `
  -ExpectedFeeds 8 -ExpectedIsoRecorders 8 -RequireObs
```

Review process drop warnings, CPU, memory, and FFmpeg counts before publishing.

Use `docs/VALIDATION_MATRIX.md` as the detailed pass/fail matrix for screen
share, Active Speaker Director, OBS lifecycle/reopen, package validation,
automated OBS smoke, and sidecar release gates.

## Documentation

- README matches current plugin capabilities.
- `CHANGELOG.md` has an entry for this version (move the `[Unreleased]`
  bullets under a new `## [X.Y.Z] - YYYY-MM-DD` section and add the compare
  link at the bottom of the file).
- `docs/CORE_PLUGIN_FUNCTIONALITY.md` matches current OBS UI.
- `docs/OPERATOR_QUICKSTART.md` is current.
- `docs/ZOOM_MARKETPLACE_OAUTH.md` matches the active production OAuth path.
- Update the Windows version in `CHANGELOG.md` and advance `MAC_VERSION` in
  `scripts/build-site.mjs` after that macOS package is public. A Windows-only
  release keeps the previous Mac pin.
- Deploy CoreVideo.io after the release is public. Run
  `node scripts/build-site.mjs && node scripts/check-site-downloads.mjs`
  and verify the live download page and operator quickstart.
- Sidecar content is clearly labeled as roadmap or architecture until it is
  production-ready.

## Publish

The Windows workflow stages these assets on a draft. A local build writes the same files to `dist/`:

- `CoreVideo-Setup-vX.Y.Z.exe` - the recommended end-user installer.
- `CoreVideo-Windows-x64-vX.Y.Z.zip` - for manual/advanced installs.
- `CoreVideo-Setup-vX.Y.Z.exe.sha256` and
  `CoreVideo-Windows-x64-vX.Y.Z.zip.sha256`.
- `CoreVideo-Windows-x64-vX.Y.Z.manifest.json` - install-layout manifest, for
  comparing a local install against the published package.

Attach `CoreVideo-Setup-vX.Y.Z.pkg` and its SHA-256 checksum for a macOS
release. Verify the Windows installer and ZIP checksums and run Gatekeeper
assessment on the signed and notarized Mac installer. Publish the draft after
the intended packages and release notes are present.

### Prereleases

The plugin update check considers newer stable releases with an uploaded
package for its platform. Testers can install a GitHub prerelease from the
release page.
