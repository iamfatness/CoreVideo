# CoreVideo Validation Matrix

Use this matrix to close the non-auth release gaps. It intentionally avoids
changing the Zoom sign-in or meeting-join path.

## Screen Share Live Validation

Run on **both** Windows and macOS. The sharer must be a *remote* participant:
the engine never subscribes to the local user's own share (by SDK design).

- Assign one output to **Screen share** before anyone shares, and put it on
  program.
- Confirm Output Manager reports `Screen share unavailable` and the source is
  **empty** (no color bars).
- Start a screen share from a meeting participant.
- Confirm the output moves through waiting-for-frame to OK. A share smaller
  or larger than 1080p is still OK — it must never read "lower resolution".
- Share a page of small text from a high-resolution screen (Retina / 1440p)
  and confirm thin strokes stay legible and do not shimmer while scrolling.
- Leave a static slide up for **two minutes**. Health must stay OK — never
  `Video stale`, no `Recovering stale` lines in the log.
- Have the sharer switch windows/apps mid-share; the picture must follow.
- Pause and resume the share from the sharer's Zoom toolbar (short and >10 s
  pauses); the picture must return after resume.
- Stop the share.
- Confirm within ~3 s the source goes **empty** (not a frozen last frame), the
  log shows `Screen share ended; cleared output`, and Output Manager returns to
  `Screen share unavailable` without changing OBS source geometry.
- Start a second share from a different participant; it must appear with no
  operator action.
- Export a support bundle and confirm `screen_share_unavailable` appears only
  when no share is active, and any `share_renderer_rebuilt` lines carry an
  `idle_ms` above 8000.

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

## Tiles Layout Animation

Every check here needs the wall on program or in a full-size preview — the
motion is the thing being validated, and a small preview hides it.

- With **Animate layout changes** off, join and leave and confirm the wall still
  changes on a single frame exactly as before.
- Toggle it **on while the wall is live** and confirm nothing moves, fades, or
  blanks: the wall should simply carry on. Toggle it **off mid-reflow** and
  confirm the wall lands on its final layout rather than freezing mid-motion.
- Join a participant and confirm the newcomer **fades in at its final slot**
  while the other tiles ease around it — it should never fly in from an edge.
- Leave a participant and confirm the wall reflows immediately, with no tile
  held on screen after the person has gone.
- Watch a **join on a wall of four or more** and confirm tiles passing one
  another during the reflow still looks acceptable on air. This is a judgement
  call, not a pass/fail: tiles are opaque and do briefly overlap in transit.
- Compare a tile **at rest against the same tile mid-motion** for any shift in
  colour, level, or sharpness. A tile in motion composites through an
  intermediate texture; at rest it does not. They must match. Check at 1:1 scene
  scale — a scaled scene resamples twice and softens by design.
- Confirm the wall **before a reflow and after it settles** is identical.
- With **Glow** enabled, join a participant and confirm the halo fades in with
  its tile rather than appearing at full strength around an invisible one.
- Change **Animation duration** across its range and confirm the wall settles
  proportionally, with no judder at the slow end.
- Run a **roster blip** — someone dropping and rejoining within a moment — and
  confirm the wall reflows out and back as a wobble. This is expected
  behaviour, not a defect.
- With eight feeds, watch CPU and GPU through several reflows and confirm no
  sustained cost outside the transitions themselves.

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
