# Tiles pending-frame handoff fix

Build: `v0.1.47-dev.2`, including the previous participant A/V ISO changes.

The reported symptom was brightness/color flashing inside faces, possibly at
speaker changes. The existing color-range normalization is present in the
installed engine and its logs show it operating. Speaker/roster updates do not
reassign a tile when its participant list is unchanged.

Code inspection found a definite failure path in the Tiles frame handoff:

1. A valid frame is waiting for the render thread (`has_frame=true`).
2. Another callback copies SHM pixels directly into that pending frame.
3. The shared-memory sequence check rejects the new copy, or its dimensions
   are unsuitable for I420. The callback returns without updating metadata.
4. The render thread still sees the previous ready flag, dimensions and frame
   generation, but the underlying pixels have been overwritten. On a resize,
   even the Y/U/V plane offsets can now describe the wrong buffer.

The fix reads into a separate reusable candidate buffer and commits pixels only
after a successful read with even dimensions. Failed reads leave the pending
valid picture untouched. It adds one retained input buffer per tile, no extra
full-frame copy, and no changes to subscriptions or color conversion.

Validation: `CoreVideoTileFrameRead` failed with direct writes to the pending
buffer, then passed after transactional publication. It covers rejected resize,
successful resize recovery, and 100 subsequent rejected reads. The full Windows
build and all 69 CTest regressions passed.

The exact connection to the observed live flashes still needs confirmation on
the new build. The previous build did not log rejected Tiles reads. The new
build logs `Tiles kept last valid frame` at the first rejection and each 300th
rejection so a live test can correlate these events. Verify repeated speaker
changes and resolution changes without visible brightness/color pops.
