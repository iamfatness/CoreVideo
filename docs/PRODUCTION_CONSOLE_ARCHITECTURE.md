# CoreVideo Plugin Production Architecture

This note documents the production-facing functionality currently exposed by
the OBS plugin. It intentionally focuses on the plugin, `ZoomObsEngine`, IPC,
source assignment, ISO recording, and TCP/OSC automation.

## Auto ISO Recording

CoreVideo keeps `ZoomObsEngine` minimal. The engine owns Zoom SDK access,
subscribes to raw participant media, writes I420/PCM into shared memory, and
emits lightweight JSON metadata over IPC. ISO recording runs in the OBS plugin
process because that side already knows output assignments, participant routing,
OBS recording state, and operator control APIs.

Flow:

1. A control surface or script assigns a Zoom output through the dock, TCP, or
   OSC.
2. `ZoomOutputManager` updates the `ZoomSource` assignment and notifies
   `ZoomIsoRecorder`.
3. `ZoomObsEngine` publishes frame/audio events with the resolved participant ID.
4. `ZoomSource` reads the shared memory frame/audio once, outputs to OBS, and
   forwards the same copied buffers to `ZoomIsoRecorder`.
5. `ZoomIsoRecorder` starts a per-output session on the first video frame,
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

## Participant Data

The plugin receives live participant metadata from `ZoomObsEngine`, including
display name, participant ID, video state, audio state, active speaker state,
spotlight index, and screen-share state. That data feeds:

- `ZoomDock` roster and output table.
- `ZoomOutputManager` source assignment and runtime reconfiguration.
- TCP `list_participants` and `list_outputs` responses.
- Active-speaker, spotlight-slot, screen-share, and fixed-participant modes.

## Automation Surface

The plugin exposes facts and commands rather than presentation decisions:

- TCP port `19870` for JSON commands.
- OSC port `19871` for broadcast controllers.
- OBS hotkeys for source-level active-speaker enable/disable.
- Output profiles for saving and restoring source-to-participant mappings.

Presentation systems should treat these APIs as the contract: assign sources,
query state, start/stop ISO recording, and let OBS render the program scene.
