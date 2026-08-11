# CLAUDE.md

Project notes for Claude Code sessions working in this repository (CoreVideo,
the OBS Studio plugin that pulls Zoom meeting video and audio into OBS).

This file did not exist before the per-participant Tiles audio feature
(2026-08-11); it was created to hold the note below, which the feature's
implementation plan calls for. Expand it as further sections warrant.

## Tiles source

Per-participant audio (`audio_group`, empty by default) auto-creates one
`zoom_participant_audio_source` per assigned tile into the nominated group.
Ownership is tracked by the `cv_tiles_audio_owner` settings marker holding the
creating Tiles source's uuid — never by name, since the operator can rename
anything. The plugin only ever touches marked sources; a name clash defers
rather than overwriting. Participants who leave the wall are muted, not
deleted. Two Tiles sources showing the same person yield one audio source, so
their voice is never doubled. Decision logic is pure in
`src/zoom-tiles-audio-plan.h` and tested in `tests/tiles-audio-plan-test.cpp`;
every scene-collection mutation lives in `src/zoom-tiles-audio.cpp`.
