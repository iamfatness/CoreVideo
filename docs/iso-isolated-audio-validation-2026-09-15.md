# Dedicated ISO audio validation

Build `v0.1.47-dev.4` includes the feed picker, Tiles handoff fix, and dedicated
participant audio recording. Both plugin and ZoomObsEngine must be installed.

Root cause: ZoomSource forwarded its embedded audio to the ISO writer. Sources
configured as Mix received mixed PCM labelled with the configured participant
ID by the engine. A participant ID alone therefore did not prove isolation.
Decoded audio envelopes from the last show's participant files were strongly
correlated (two pairs 0.9956 and 0.9467), consistent with this path.

ISO now owns an audio-only subscription per recorded participant. Source PCM
is never passed to the writer. The engine forces these subscriptions to receive
only that participant's one-way audio and excludes them from the set of output
routes that claim participants away from Audience audio. Per-slot attribution
is validated again at the recorder's dedicated SHM reader.

Validation completed:

- Full Windows build and 72 CTest regressions passed.
- Routing regression rejects mix and other participants while retaining normal
  isolated, meeting-mix, and Audience behavior.
- Real SHM reader regression retains first/coalesced buffers, does not duplicate
  drained buffers, clears notifications, and rejects another participant ID.
- End-to-end synthetic alternating speakers through the production routing
  policy, real shared-memory reader, resampler, and FFmpeg MP4 writers passed.
  Decoded one-second RMS for participant 1: 8490.78, 0, 8485.57, 0.
  Participant 2: 0, 8482.77, 0, 8482.81. Non-speaking intervals were digital silence.
  Both files decoded completely without errors.

Reproduce: run `CoreVideoIsoIsolatedRecordingTest <ffmpeg>` in an empty directory,
then `python tests/verify-iso-isolated.py <ffmpeg> <directory>` from the repo root.
These are synthetic SDK-input tests, not a live Zoom meeting. Live acceptance
still requires two people alternating speech and checking their respective MP4s.
Existing mixed recordings are unchanged; this fix cannot recover original
isolated stems that were never captured.
