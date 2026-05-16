# Sidecar OBS Control Issue List

## Issue 1 - Centralize Look-to-OBS rendering
Status: Completed

Create a single renderer path that translates a `Look` into OBS scenes, sources, scene items, backgrounds, overlays, and program/preview routing. `MainWindow` should stop owning OBS scene composition details directly.

Acceptance criteria:
- One sidecar component owns Look scene names, source names, and render/provision calls.
- Rendering a Look creates or updates the matching OBS scene.
- Provisioning uses the same path as manual render/take.
- Existing sidecar tests still pass.

## Issue 2 - Mirror tile design into OBS
Status: In progress

The Look Designer currently updates sidecar previews. OBS must receive equivalent visual layers.

Acceptance criteria:
- Canvas color appears in OBS when no background image is selected.
- Tile border layers are created and transformed in OBS.
- Name tag text sources are created and positioned in OBS.
- Tile opacity and shadows have an OBS-side representation or explicit unsupported status.

Progress:
- Canvas color is rendered as a deterministic bottom layer.
- Background image is rendered above canvas color and disabled when no image is selected.
- Tile shadows are rendered as per-slot offset color layers.
- Tile borders are rendered as per-slot color layers.
- Tile opacity is approximated with per-slot dim overlays.
- Name tags are rendered as per-slot OBS text layers.
- Stale border, shadow, dim, and name layers are disabled when the Look no longer uses them.
- Layer order is deterministic: canvas, background, shadows, borders, participant slots, dim overlays, name tags, overlays.

Remaining:
- True rounded-corner masking still needs an OBS-side matte/filter strategy.

## Issue 3 - Add OBS read-back sync status
Status: In progress

After applying a Look, query OBS and show whether the sidecar model matches the OBS scene graph.

Acceptance criteria:
- Each rendered Look has `Synced`, `Dirty`, `Applying`, or `Error` state.
- Missing scenes/sources/items are surfaced in the UI.
- Failed OBS websocket requests are visible without opening logs.

Progress:
- Rendering a Look now puts the top-bar sync indicator into `Applying`.
- The normal inventory read-back refresh restores aggregate sync counts after OBS has processed the render.
- Single request and batch request failures now emit a sidecar signal and surface as `Sync error` in the top bar tooltip.
- The top-bar sync indicator now tracks explicit `Offline`, `Applying`, `Synced`, `Dirty`, and `Error` states.
- The sync tooltip records the last rendered Look and OBS scene.
- The sync check now compares expected design layers for the last rendered Look against cached OBS scene items.
- Missing canvas/background/shadow/border/dim/name sources are surfaced in the sync tooltip as missing design layers.

## Issue 4 - Make the Look Designer a first-class workspace
Status: Pending

Move the designer from a modal dialog into a real editor surface inside the sidecar.

Acceptance criteria:
- Large editable program canvas is always visible in design mode.
- Right inspector controls canvas, tile, overlay, and participant-display settings.
- Left side can select saved Looks/galleries.
- Designer changes can push to OBS preview or program.

## Issue 5 - Add direct layout editing
Status: Pending

Allow resizing/repositioning participant tiles on the canvas and save the result as a reusable Look/template.

Acceptance criteria:
- Drag tile to move.
- Resize tile handles.
- Snap/grid controls.
- Save custom geometry with the Look.
- OBS scene updates use the edited geometry.

## Issue 6 - Harden participant mapping
Status: Pending

Participant mapping must be source-based, not sidecar-preview-only.

Acceptance criteria:
- Zoom participants with video are assigned to stable `CoreVideo Slot N` outputs.
- Placeholder participants exist when no Zoom meeting is joined.
- Participant names replace placeholders after join.
- Joining/leaving does not rebuild Looks or break OBS scene references.

## Issue 7 - Add moderation controls inspired by Tiles
Status: Pending

Add operational participant controls for gallery outputs.

Acceptance criteria:
- Favorite participant.
- Block participant from gallery.
- Filter by video status.
- Sort by role/video/name.
- Rotation queue for large meetings.

## Issue 8 - Add deterministic OBS integration tests
Status: Pending

Create repeatable test coverage for OBS provisioning and Look rendering.

Acceptance criteria:
- Empty OBS profile creates all expected CoreVideo scenes/sources.
- Saving/reloading a custom Look preserves styling.
- Rendering a Look updates the OBS scene graph.
- TAKE switches OBS program scene.
