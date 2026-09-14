# ISO buffering and timing repair

The four-frame FFmpeg input queue rejected ordinary encoder startup and catch-up
bursts. Eight real 1080p NVENC encoders on the owner's RTX 4090 reproduced startup
loss with the previous queue, including when the comparison order was reversed.
The video pacer consumed the rejected frame slots, so a continuing rawvideo track
silently lost elapsed time. CoreVideo Pro PR #531 addressed the same startup/jitter
capacity problem; its serial Media Foundation writer and CPU resize fixes do not
map directly to this recorder's existing independent FFmpeg processes.

## Behavior

- Each pipe accepts 96 outstanding pictures during startup, then 64 after the first
  successful pipe write and backlog reduction to the steady bound. These are pipe
  delivery measurements, not proof that the codec has emitted an encoded packet.
- Outstanding work includes the in-flight write. A batch holds one pixel buffer
  plus a repetition count. All repetitions count against the picture limit; the
  pixel allocation is charged once against the separate byte limit.
- Recorder pixel storage is bounded to the smaller of 96 native frames or 288 MiB
  per track. Eight 1080p tracks could retain about 2.23 GiB at startup / 1.48 GiB in
  steady state, excluding the incoming frame, process/codec buffers and other app
  memory. Normal occupancy was much lower in the finite tests below.
- Catch-up batches are accepted atomically, and only acceptance advances the
  recording clock. A rejected batch stops that source for the rest of the recording
  run, signals EOF, and lets accepted video drain. It does not silently continue a
  shortened timeline or repeatedly spawn replacement encoders. Other tracks and
  the meeting continue. This deliberately produces a visibly incomplete track on
  sustained overload or a gap exceeding the bounded allowance.
- Stopped tracks reject subsequent audio too. Their WAV headers are finalized by
  status polling or Stop. Already accepted audio may extend beyond the partial
  video's endpoint; this is a failed take, not an A/V-continuity guarantee.
- Queue time, bytes, logical occupancy, peak occupancy, startup state, actual pipe
  writes, and incomplete-track state are exposed in ISO status. Errors survive
  finalization; a run-level warning persists even if older rows leave the history.
- Normal video callbacks no longer open/read the FFmpeg log file. Status polling
  and exceptional failure handling retain the diagnostic tail.

## Verification

The Windows Release OBS plugin builds against the available OBS/Qt dependencies.
The Zoom engine and sidecar were disabled for this plugin build. Optional internal
FFmpeg hardware acceleration was disabled, matching the locally observed v0.1.45
plugin configuration; the ISO recorder's external FFmpeg NVENC path remains active.

The native test suite has 66 tests. Added regressions exercise atomic rejection,
clock commit on acceptance, compact catch-up, independent frame/byte bounds,
in-flight accounting, startup-to-steady transition, EOF draining, retained peak
counts, and termination while a repeated picture is blocked in the pipe.

`CoreVideoIsoRecordingTest` is a manual real-codec harness. In an empty directory:

```
CoreVideoIsoRecordingTest <absolute-ffmpeg-path> h264_nvenc 8
```

Two consecutive ten-second takes, eight 1080p30 NVENC writers each, contain a
500 ms input gap per take and changing synthetic luma patterns. All 16 MP4s:

- accepted and wrote exactly 300 pictures, with a peak queue of 16 pictures;
- finalized with FFmpeg exit 0;
- independently probed as exactly 300 decoded frames and 10.000 seconds;
- decoded fully without errors.

The harness uses real pipe and pacer code, but does not exercise the OBS/Zoom
callback wiring or WAV recorder. It is not a live-show capacity certification.
Proof files and JSON are under the ignored `build-iso/recording-proof` directory.

## Operator acceptance and release scope

On 2026-09-14 the operator confirmed that the live-soak and lip-sync checks were
good and authorized publishing a new release. This is operator-reported acceptance;
no additional live-run metrics were supplied. The release target is v0.1.46.
Windows packaging includes the plugin and Zoom engine/runtime. Signed macOS
packaging still requires the maintainer's Mac; this Windows release does not
claim a new signed macOS installer.
