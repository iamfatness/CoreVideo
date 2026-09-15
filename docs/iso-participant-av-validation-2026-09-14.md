# Continuous participant ISO recording validation

Development build: `v0.1.47-dev`. This change has not been installed into the
running OBS instance or published as a release.

## Change

The previous recorder opened separate video MP4 and audio WAV files and keyed
video lifetime to source/resolution changes. The replacement owns one writer
per Zoom participant ID for the Record/Stop interval. Duplicate source UUIDs
share that writer. Input size changes only reconfigure the scaler.

Every participant MP4 contains 1920x1080/30 H.264 and 48 kHz stereo AAC.
Aspect ratio is preserved with black bars. A worker per participant performs
scaling, resampling and timestamped A/V transport over one FFmpeg stdin pipe.
Video gaps hold the picture; audio gaps contain silence. A participant with no
media callbacks still gets black video and silence. No merge step or automatic
replacement segment is created. Diagnostic text logs remain beside the MP4.

The Windows GPU scaler and aspect-fit geometry were adapted from CoreVideo
Pro's `D3DIsoFrameConformer.h` and `IsoFrameConform.h`. This recorder uses planar
I420 rather than Pro's NV12 output, and preserves the engine's full-range
BT.709 pixels. Other platforms use the OBS video scaler.

## Completed checks

- Full Windows plugin/engine build succeeded; 68 CTest regressions passed.
- Real worker fixtures: two simultaneous libx264 participant encoders and eight
  simultaneous NVENC participant encoders on this RTX 4090 host. Each run also
  had a silent/no-video libx264 writer and a deliberately invalid encoder process.
- Every participant produced one MP4 with both tracks, 180 decoded video frames
  over six seconds, fixed 1920x1080/30 video, stereo 48 kHz AAC, and no decode errors.
- Inputs changed between 640x360, 1280x720, 1920x1080, and 640x480, followed by a
  camera-off second and recovery. Decoded pixels verified pillarboxing, recovery
  to full canvas, and separate U/V colors through scaling and native-size input.
- Input audio changed from 32 kHz mono to 48 kHz stereo, with callbacks withheld
  outside test tones. Clustered callback timestamps verified that PCM placement
  never rewinds or overwrites a previously received batch.
- Decoded flashes/tones stayed within 20 ms on both encoders; all six 100 ms
  audio tones retained their duration. No late audio samples were reported.
- No-input tracks decoded as continuous black and silent AAC. The failed encoder
  reported an error without stopping the healthy recordings.
- Provider-policy regression covers duplicate source suppression, monotonic
  timestamps, alternate takeover after a gap, and source removal.

Validation found and corrected startup history loss, fragmented MP4 startup
offset with B-frames, input color metadata, planar chroma conversion, and audio
rewinding on bunched arrival timestamps.

## Reproduce

Build `CoreVideoIsoTrackWriterTest` with the Windows plugin dependencies. Run
from an empty directory, with OBS's runtime DLL directory on PATH:

```powershell
& $writerTest $ffmpeg h264_nvenc 8
python tests/verify-iso-av.py $ffmpegBin $fixtureDirectory
```

Run the verifier from the repository root. Use `libx264 2` for the CPU fixture.
The test refuses to overwrite existing MP4s. The verifier checks decoded media,
not only FFmpeg exit status. Local final results are in `build-iso/av-burst-cpu`
and `build-iso/av-burst-nvenc`; regression results are in `build-iso/av-ctest.log`.

## Remaining release validation

- Live Zoom soak, including repeated resolution changes, mute/camera toggles,
  duplicate OBS sources, source reassignment/removal, Stop, and a new Record run.
- Live lip-sync check, sustained resource usage at the intended participant
  count, and playback/import in the production editor.
- Hosted macOS/Linux builds and platform recording checks; local validation
  above is Windows only.

The exposed identity is Zoom's numeric participant ID. A rejoin that receives a
new ID creates a new participant file; matching by display name would risk
combining different people. Known fixed participants share the Record epoch;
late participants expose their offset through `start_offset_ms` status.
