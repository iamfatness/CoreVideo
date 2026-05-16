# Core Plugin Functionality

This guide covers the main CoreVideo OBS plugin workflows. It intentionally
focuses on the OBS plugin, `ZoomObsEngine`, Zoom source assignment, control APIs,
audio routing, and ISO recording.

## Media Architecture

![CoreVideo plugin media pipeline](images/core-plugin-pipeline.svg)

CoreVideo keeps all Zoom Meeting SDK access inside the lightweight
`ZoomObsEngine` helper process. The OBS plugin starts the engine, joins the
meeting, and sends subscription requests over JSON IPC. Video and audio payloads
move through named shared memory so large frames are not copied through the IPC
pipe.

The plugin-side `ZoomSource` reads the shared memory frame, outputs it into OBS,
and forwards the same copied buffer to optional plugin-side services such as ISO
recording. This keeps the engine minimal and makes OBS/plugin features easier to
test independently from the Zoom SDK.

## What It Looks Like in OBS

The CoreVideo plugin is operated from inside OBS. The main pieces operators see
are the Zoom Control dock, regular OBS scenes/sources, the Zoom Output Manager,
and the Zoom Participant source properties.

![CoreVideo OBS workspace with Zoom Control dock](images/corevideo-obs-workspace.svg)

The Zoom Control dock joins and leaves meetings, starts and stops raw media, shows
meeting state, lists participants, and shows output assignments without leaving
OBS.

![CoreVideo Output Manager](images/corevideo-output-manager.svg)

The Output Manager maps actual Zoom participant names to OBS sources and shows
requested resolution, observed signal, frame rate, and audio routing.

![CoreVideo Zoom Participant source properties](images/corevideo-source-properties.svg)

Each Zoom Participant source can be configured independently for fixed
participants, active speaker, spotlight slot, screen share, isolated audio,
audience audio, resolution, video-loss behavior, and hardware conversion.

## Joining Meetings

1. Open OBS.
2. Open **Tools > Zoom Control**.
3. Enter a Zoom meeting ID or full Zoom join URL.
4. Enter a display name.
5. Use **Auto Zoom sign-in / ZAK** unless Zoom support gives you a specific ZAK.
6. Click **Join**.
7. Click **Start Engine** after joining to request raw media from Zoom.

For external-account meetings, configure OAuth in **Tools > Zoom Plugin
Settings**. CoreVideo uses the OAuth login to fetch a short-lived ZAK from
`/v2/users/me/zak` and passes that ZAK to the Meeting SDK join parameters.

## Source Assignment

Add one or more **Zoom Participant** sources in OBS. Each source can follow a
different assignment mode:

| Mode | Behavior | Common Use |
|---|---|---|
| Participant | Fixed Zoom participant ID | Dedicated guest ISO |
| Active Speaker | Follows the current active speaker | Host/speaker-follow shot |
| Spotlight Slot | Follows Zoom spotlight position 1..N | ZoomISO-style production |
| Screen Share | Follows active screen share | Slides/demo capture |

Each output reports observed resolution and frame rate through the output
manager and TCP `list_outputs` command.

## Audio Routing

CoreVideo supports three audio modes for participant outputs:

| Routing | Behavior |
|---|---|
| Mixed | Full meeting mix |
| Isolated | Only the assigned participant's one-way audio |
| Audience | Residual one-way audio for participants not assigned to isolated outputs |

Use **Isolated** when you need the assigned participant only. Use **Audience**
for a remaining-room or overflow mic channel after dedicated isolated sources
have claimed named participants.

## Auto ISO Recording

![CoreVideo ISO recording flow](images/iso-recording-flow.svg)

ISO recording is controlled by the OBS plugin, not the engine. When enabled,
CoreVideo records one video file and one PCM WAV audio file per active source
segment. A new segment starts when the resolved participant or source resolution
changes.

Requirements:

- `ffmpeg` must be available on `PATH`, or pass an explicit `ffmpeg_path`.
- Raw media must be active.
- Sources must be assigned to participant, active speaker, or spotlight modes.

TCP start example:

```json
{"cmd":"iso_recording_start","output_dir":"C:/Recordings/CoreVideo","ffmpeg_path":"ffmpeg","record_program":true}
```

TCP status example:

```json
{"cmd":"iso_recording_status"}
```

TCP stop example:

```json
{"cmd":"iso_recording_stop"}
```

OSC equivalents:

| Address | Type tags | Action |
|---|---|---|
| `/zoom/iso/start` | optional `,s` | Start ISO recording, optional output directory |
| `/zoom/iso/stop` | none | Stop ISO recording |

Output files are written as:

- `*.mp4` for encoded I420 video through FFmpeg/libx264
- `*.wav` for matching PCM audio

When `record_program` is true, CoreVideo also starts the normal OBS program
recording and stops it when ISO recording stops, but only if CoreVideo started
that OBS recording session.

## TCP Control Examples

All TCP commands are newline-delimited JSON sent to `127.0.0.1:19870`.

List participants:

```json
{"cmd":"list_participants"}
```

List outputs:

```json
{"cmd":"list_outputs"}
```

Assign a source to a fixed participant with isolated mono audio:

```json
{"cmd":"assign_output_ex","source":"Zoom Participant 1","mode":"participant","participant_id":123456,"isolate_audio":true,"audio_channels":"mono","video_resolution":"1080p"}
```

Assign a source to active speaker:

```json
{"cmd":"assign_output_ex","source":"Zoom Participant 2","mode":"active_speaker","audio_channels":"mono","video_resolution":"1080p"}
```

Assign a source to spotlight slot 1:

```json
{"cmd":"assign_output_ex","source":"Zoom Participant 3","mode":"spotlight","spotlight_slot":1,"audio_channels":"mono","video_resolution":"1080p"}
```

## OSC Control Examples

List participants and outputs:

```text
/zoom/list_participants
/zoom/list_outputs
```

Assign a source:

```text
/zoom/output/assign_ex "Zoom Participant 1" "participant" 123456 1
```

Assign active speaker:

```text
/zoom/assign_output/active_speaker "Zoom Participant 1"
```

Set isolated audio:

```text
/zoom/isolate_audio "Zoom Participant 1" 1
```

Start and stop ISO recording:

```text
/zoom/iso/start "C:/Recordings/CoreVideo"
/zoom/iso/stop
```

## Troubleshooting

| Symptom | Check |
|---|---|
| Color bars only | Confirm the meeting is joined, raw media is started, and the source has a participant/role assignment. |
| No isolated audio | Confirm the source is assigned to a real participant and `isolate_audio` is true. |
| ISO recording does not start | Confirm `ffmpeg` is on PATH or provide `ffmpeg_path`. |
| External meeting rejected | Confirm the Meeting SDK app/client ID is approved or published for external meeting joins. |
| Plugin cannot launch engine | Confirm `ZoomObsEngine.exe` and Zoom SDK runtime DLLs are installed under `obs-plugins/64bit/zoom-runtime`. |
