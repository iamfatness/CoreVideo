# CoreVideo Validation Matrix

Use this matrix to close the non-auth release gaps. It intentionally avoids
changing the Zoom sign-in or meeting-join path.

## Screen Share Live Validation

- Assign one output to **Screen share** before anyone shares.
- Confirm Output Manager reports `Screen share unavailable`.
- Start a screen share from a meeting participant.
- Confirm the output moves through waiting-for-frame, then OK or lower-quality
  health depending on the delivered feed.
- Stop the share.
- Confirm the output returns to `Screen share unavailable` without changing OBS
  source geometry.
- Export a support bundle and confirm `screen_share_unavailable` appears only
  when no share is active.

## Active Speaker Director Live Validation

- Use at least three participants with video enabled.
- Confirm sensitivity prevents immediate cuts to short interruptions.
- Confirm hold time blocks rapid back-and-forth switching.
- Toggle **Require video** and verify audio-only talkers are ignored when it is
  enabled.
- Add an exclusion for a current speaker and verify the director moves to an
  eligible speaker.
- Use manual take/release and confirm release does not cut away unnecessarily
  when the manual speaker remains valid.

## Tiles Wall Live Validation

- Use at least four participants with video enabled.
- In `Auto - everyone with video`, confirm the wall relays itself as
  participants join and leave, and that it never exceeds **Maximum tiles**.
- Add a participant to **Never show** and confirm they leave the wall and no
  gap is left behind.
- Switch to `Manual - choose per tile`, assign specific people, and confirm
  each tile holds its assignment across a join/leave.
- Change **Tile shape** and confirm tiles crop to fill rather than letterbox.
- Change **Gap between tiles** and **Margin around the wall** and confirm the
  spacing scales with canvas height.
- Set a **Background source** and confirm it draws behind the tiles; delete that
  source mid-show and confirm the wall falls back to **Background colour**
  instead of breaking.
- Point **Background source** at the wall itself, or a scene containing it, and
  confirm the self-reference falls back to colour.
- Enable borders and glow; confirm an extreme **Border width** or **Corner
  radius** clamps instead of inverting the tile.
- Apply a per-tile **crop left %** / **crop right %** and confirm only that tile
  changes.
- Set **Participant audio scene or group** and confirm one audio source per tile
  is created inside it, each with its own fader and ISO track, and that the wall
  itself carries no audio.
- Cut between scenes with the audio scene/group present in each and confirm
  audio does not drop.
- Add a second Tiles source with an audio group set and confirm the plugin logs
  a warning about the second wall.

## OBS Lifecycle And Reopen

- Start OBS with the plugin installed.
- Open Zoom Control, Output Manager, Diagnostics, and ISO Recorder.
- Close and reopen each dock from **Tools**.
- Create a participant source, active-speaker source, screen-share source, and
  participant-audio source.
- Close OBS from the normal Exit menu.
- Reopen OBS and confirm the same sources load without crash dialogs.
- Validate lifecycle log markers:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -LogOnly -ExpectDockShow -ExpectShutdown `
  -ObsLogPath "$env:APPDATA\obs-studio\logs\YYYY-MM-DD HH-MM-SS.txt" `
  -ExpectedDockId ZoomControlDock,ZoomOutputManagerDock,ZoomDiagnosticsDock,ZoomIsoRecorderDock
```

## Installer And Release Package

- Build or stage a package.
- Validate core runtime and optional full Zoom runtime:

```powershell
.\scripts\Test-CoreVideoPackage.ps1 -PackageRoot .\dist\CoreVideo-Windows-x64 `
  -FullRuntime -ManifestPath .\dist\CoreVideo-Windows-x64.manifest.json
```

- If sidecar is included, the validator also checks required template/look JSON.
- If secret env vars are present, the validator fails if their values appear in
  the plugin DLL.

## Automated OBS Smoke

Use this when OBS WebSocket is enabled:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -VerifyCoreVideoPlugin -CreateCoreVideoInputs `
  -ParticipantCount 8
```

This confirms CoreVideo source kinds are registered and can be instantiated,
then creates an 8-slot scene plus a screen-share scene.

## Sidecar Gate

Sidecar remains a lower-priority production surface. Before presenting it as
release-ready:

- `ctest --test-dir build-vs-release -C Release --output-on-failure` must pass
  all sidecar tests.
- Packaged sidecar builds must include templates and looks validated by
  `Test-CoreVideoPackage.ps1`.
- Run an OBS WebSocket smoke pass against the sidecar-created scenes before
  publishing sidecar-facing release notes.
