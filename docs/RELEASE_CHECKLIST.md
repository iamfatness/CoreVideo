# CoreVideo Release Checklist

Use this checklist before publishing a GitHub Release or installer.

## Build Inputs

- `main` is up to date and all intended commits are pushed.
- Zoom SDK runtime is available for the release job.
- Windows build embeds the production Public Client ID:
  `y6sIWSwiTZe1JygMx4C9EQ`.
- Windows build embeds the same value as the Meeting SDK public app key.
- No OAuth or Meeting SDK client secret is embedded in the plugin binary.
- FFmpeg runtime strategy is documented for ISO recording.

## Local Validation

Run:

```powershell
cmake --build build-vs-release --config Release --target obs-zoom-plugin --parallel
ctest --test-dir build-vs-release -C Release --output-on-failure
git diff --check
```

For a local package:

```powershell
.\scripts\release-local.ps1 -Version vX.Y.Z -SkipUpload
```

Confirm `scripts/Test-CoreVideoPackage.ps1` validates:

- OBS plugin DLL.
- OAuth callback helper.
- Qt runtime and TLS plugins.
- Locale data.
- Zoom runtime files for full releases.
- Embedded OAuth public client ID.
- Embedded Meeting SDK public app key.
- FFmpeg runtime consistency when FFmpeg DLLs are present.
- A deterministic install-layout manifest:
  `CoreVideo-Windows-x64-<version>.manifest.json` locally or
  `CoreVideo-Windows-x64.manifest.json` in GitHub Actions.

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
- `docs/CORE_PLUGIN_FUNCTIONALITY.md` matches current OBS UI.
- `docs/OPERATOR_QUICKSTART.md` is current.
- `docs/ZOOM_MARKETPLACE_OAUTH.md` matches the active production OAuth path.
- Published website at `corevideo.io` (with `corevideo.iamfatness.us` as a live
  alias) is deployed from the same commit as the release.
- Sidecar content is clearly labeled as roadmap or architecture until it is
  production-ready.

## Release Assets

Publish:

- Windows installer, when NSIS packaging succeeds.
- Windows ZIP for manual/advanced installs.
- SHA256 files for every downloadable asset.
- Install-layout manifest JSON for comparing local and GitHub release contents.
- Release notes with known limitations and upgrade notes.
