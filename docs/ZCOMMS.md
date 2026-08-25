# ZComms — what CoreVideo owes it

ZComms is a standalone intercom built on the Zoom Meeting SDK. It is a
**separate product in its own repository**, sharing this repo's engine. Its
architecture, phasing and admin model live there, not here.

This file records only the parts that are *CoreVideo's* work, so nobody in this
repo has to reverse-engineer why they are coming.

## The shared surface

ZComms does not have its own Zoom SDK integration. It builds and runs the same
engine this plugin does. The surface both products depend on:

- `engine/` — `main.cpp`, `engine-audio`, `engine-video`, `engine-share`,
  `engine-writer`.
- The headers both sides must agree on byte-for-byte: `src/engine-ipc.h`,
  `src/shm-generation.h`, `src/audio-timeline.h`, `src/audio-silence-fade.h`,
  `src/media-event-queue.h`.

Nothing else — sources, docks, the tile compositor, the ISO recorder — is
shared. ZComms consumes this repo as a pinned submodule while the audio-send
path is new; the intended end state is a shared core repo, and the extraction
boundary is already `engine-ipc.h`.

## One audio-send implementation

The Meeting SDK's TX path — `IZoomSDKAudioRawDataHelper::setExternalAudioSource`
and `IZoomSDKAudioRawDataSender::send` — is vendored in
`third_party/zoom-sdk/h/rawdata_audio_helper_interface.h` and has never been
called by this codebase. Both the plugin's talkback and ZComms hang off it.

**It gets exactly one implementation, in the engine.** Two front ends, one send
path. The plugin's talkback work should consume `engine-talkback` rather than
growing its own; a second send path means two sets of divergent timing bugs in
the hardest code in the tree to reproduce a fault in.

Two design constraints that belong with it, both already paid for here:

- The mic ring (app → engine) is **pulled on a fixed 20 ms cadence** and uses no
  notify flag. Zoom wants a steady stream anyway, and a paced puller removes the
  reader-wedge failure class the edge-triggered protocol carries. Underrun
  becomes a countable condition rather than a silence bug.
- PTT press and release ramp through `audio-silence-fade.h`. A hard gate is
  exactly the true-zero-PCM transition that helper exists for, and it clicks.

## The prerequisite: namespace the engine by owner

`terminate_stale_engine_processes()` in `src/zoom-engine-client.cpp` kills
**every** `ZoomObsEngine.exe` on launch. That is correct today — it is what
fixed the 2026-08-17 ghost-writer defect, where an orphaned engine poisoned the
notify flag and cost ~92% of audio with no error anywhere.

It also makes ZComms impossible. The intercom runs alongside OBS by design, and
one engine per channel means several engines at once; as written they terminate
each other.

The fix is to scope both the pipe names and the stale-process scan by an owner
id — engines tagged on the command line, each launcher matching only its own
tag — and to make the region prefix part of the same change. Every shared name
is hardcoded to `ZoomObsPlugin_` today (pipes `_P2E`/`_E2P`, regions `_video`
/`_audio`/`_share`), and a second product writing under this plugin's prefix
recreates the ghost-writer condition exactly. One change, one prefix scheme, and
a test pinning "engine A's launch does not kill engine B."

**This lands in this repo, and it blocks ZComms shipping at all.**

## The SDK asset is reachable from here only

`third_party/zoom-sdk/` is gitignored and fetched in CI from a release asset on
*this* repository (`repos/${GITHUB_REPOSITORY}/releases/assets/...` in
`.github/workflows/build.yml`, served by the private `sdk-assets` release). A
default token in another repository cannot read it.

So ZComms CI needs either a token or App installation with read access to this
repo's releases, or a second copy of the asset published privately on its own
side. Worth knowing here because the asset and the workflow that publishes it
are maintained in this repo.

Note the split: the SDK *headers* are committed (32 files under
`third_party/zoom-sdk/h/`); only the binaries and framework are fetched.
