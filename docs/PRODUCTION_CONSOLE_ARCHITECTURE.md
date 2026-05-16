# CoreVideo Production Console Architecture

## Priority 1: Auto ISO Recording

CoreVideo keeps `ZoomObsEngine` minimal. The engine only owns Zoom SDK access,
subscribes to raw participant media, writes I420/PCM into shared memory, and
emits lightweight JSON metadata over IPC. ISO recording runs in the OBS plugin
process because that side already knows output assignments, participant routing,
OBS recording state, and operator control APIs.

Flow:

1. Sidecar or another controller assigns a Zoom output through TCP/OSC.
2. `ZoomOutputManager` updates the `ZoomSource` assignment and notifies
   `ZoomIsoRecorder`.
3. `ZoomObsEngine` publishes frame/audio events with the resolved participant id.
4. `ZoomSource` reads the shared memory frame/audio once, outputs to OBS, and
   forwards the same copied buffers to `ZoomIsoRecorder`.
5. `ZoomIsoRecorder` lazily starts a per-output session on the first video frame,
   rotates the session if the resolved participant or resolution changes, writes
   I420 video to FFmpeg, and writes PCM audio to a WAV file beside it.
6. If requested, `ZoomIsoRecorder` starts the normal OBS program recording for
   the main program output.

Control API:

- TCP `{"cmd":"iso_recording_start","output_dir":"...","ffmpeg_path":"ffmpeg","record_program":true}`
- TCP `{"cmd":"iso_recording_stop"}`
- TCP `{"cmd":"iso_recording_status"}`
- OSC `/zoom/iso/start [,s output_dir]`
- OSC `/zoom/iso/stop`

Output:

- One `.mp4` encoded video file per source/participant/resolution segment.
- One `.wav` PCM audio file per matching segment.
- Normal OBS program recording when `record_program` is true.

## Priority 2: Participant-Synced Lower Thirds

Starter code lives in `sidecar/src/lower-third-controller.*`.

The controller generates deterministic `cv-auto-lt-*` lower-third overlays from
the current staged Look, live Zoom participant roster, and slot assignments.
MainWindow now refreshes these generated overlays when participants update or a
slot assignment changes. Manual overlays remain untouched; only generated
`cv-auto-lt-*` overlays are replaced.

Next production steps:

- Add UI for per-participant manual name/subtitle overrides.
- Persist lower-third template settings and overrides in the show profile.
- Mirror lower-third changes to OBS immediately for the active PGM scene, not
  only on explicit render/take.

## Priority 3: Director Console, Templates, Automation

Starter types live in `sidecar/src/director-automation.h`.

The intended console model is:

- Roster: live Zoom participants, video state, audio state, speaker state, slot.
- Template bus: PVW/PGM Looks with PiP, grid, talk-show, speaker+screenshare.
- Automation: active-speaker sensitivity, hold time, optional auto-take, and
  manual supersede.
- Hotkeys: TAKE, AUTO, FTB already exist; the next layer should add slot select,
  assign selected participant, speaker supersede, and lower-third toggle.

The automation should remain Sidecar-owned. The plugin should expose facts and
commands; Sidecar should decide production intent.
