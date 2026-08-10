# CoreVideo Tiles — Roster-Driven Participant Picker

**Date:** 2026-08-10
**Status:** Approved, ready for implementation planning
**Supersedes:** the `participants` editable string list in
`docs/superpowers/specs/2026-08-09-corevideo-tiles-design.md`

## Problem

Phase 1 shipped the `CoreVideo Tiles` source with an assignment list built from
`obs_properties_add_editable_list(OBS_EDITABLE_LIST_TYPE_STRINGS)` — the operator
types raw numeric Zoom user IDs, one per tile, in tile order.

Verified live on 2026-08-10: the source registers, instantiates, and reports its
canvas, but the assignment UI is unusable. Zoom user IDs are opaque 8–10 digit
numbers that change every meeting and appear nowhere in the OBS UI. Casting a
six-person wall means finding six IDs in a log and typing them without a
transcription error, while the show is starting.

Every other CoreVideo source already solved this. `zoom-source.cpp:1802` and
`zoom-participant-audio-source.cpp:334` both build a combo box from the live
roster. Tiles is the one source that does not, and it is the one source that
needs the most assignments.

## Decisions

| Question | Decision |
|---|---|
| Auto-fill or explicit casting? | **Both.** Auto is the default; Manual is an override. |
| Camera off mid-show? | **Drop the tile and reflow immediately** (Zoom-gallery behavior). Auto mode only — see "Resolver contract" for why Manual deliberately differs. |
| Maximum tiles | **9** (3×3), matching the Phase 1 definition of done. |
| Existing `participants` setting | **Removed.** The source has never shipped; there is no installed base and no reason to carry a typed-ID path forward. |

**Accepted cost of immediate reflow.** Every camera toggle relayouts the whole
wall while on air: a 4-up becomes a 3-up and every remaining face moves and
changes size. This was chosen deliberately over holding a dark tile, because it
matches what operators expect from a gallery. The stability rule in
"Resolver contract" below limits the damage to the minimum the choice allows —
remaining participants never *reorder*, they only close up.

## Architecture

Three pieces, one of which holds all the logic worth testing:

| File | Responsibility | Depends on |
|---|---|---|
| `src/zoom-tile-fill.h` (new) | Pure resolver: previous wall + roster + params → next wall | `zoom-types.h` only |
| `tests/tile-fill-test.cpp` (new) | Resolver unit tests | the resolver |
| `src/zoom-supersource.cpp` (modify) | Properties, roster callback, feeds the existing `FeedPlan` diff | OBS, engine client |
| `CMakeLists.txt` (modify) | Register `CoreVideoTileFillTest` in the `BUILD_TESTING` block | — |

`zoom-types.h` is dependency-free (`<cstdint>`, `<functional>`, `<string>`), so
the resolver uses the real `ParticipantInfo` rather than a parallel struct, and
the test still builds in the plugin-OFF/engine-OFF/Qt-less configuration that
CI's portability job uses.

## Properties

A `Fill mode` list (`OBS_COMBO_TYPE_LIST` / `OBS_COMBO_FORMAT_INT`) selects the
group; `obs_property_set_visible` from a modified-callback hides the other, so
the dialog only ever shows the controls in play.

**Auto mode**
- `max_tiles` — int, 1–9, default 9.
- `exclude_1` … `exclude_3` — roster lists with a `None` entry. Three, because
  OBS properties have no multi-select control: one covers the host account that
  runs the plugin, three covers host + producer + tech. Anything more elaborate
  is what Manual mode is for.

**Manual mode**
- `tile_1` … `tile_9` — roster lists with a `None` entry, in tile order.

**Shared**
- `canvas_width` / `canvas_height` — unchanged from Phase 1.
- `Refresh participants` button — the same no-op-returning-true button the other
  two sources use to force a property rebuild.

Every roster list is built identically to the existing sources: a `None` entry
at value 0, then one entry per participant labelled `Name (id)`, or `ID <n>` when
the display name is empty, with a `[video]` marker appended. Values are the
`uint32_t` user id widened to `long long`.

## Resolver contract

```cpp
enum class TileFillMode { Auto = 0, Manual = 1 };

struct TileFillParams {
    TileFillMode          mode      = TileFillMode::Auto;
    std::size_t           max_tiles = 9;
    std::vector<uint32_t> excluded;   // Auto only; zeros ignored
    std::vector<uint32_t> manual;     // Manual only; zeros are empty slots
};

std::vector<uint32_t> resolve_tile_assignments(
    const std::vector<uint32_t>      &previous,
    const std::vector<ParticipantInfo> &roster,
    const TileFillParams             &params);
```

**Auto rules, in order:**

1. Take every roster entry with `has_video == true` that is not in `excluded`.
2. **Stability first:** emit the members of `previous` that survived rule 1, in
   their existing order. This is the rule that makes the wall usable — without
   it, any roster reordering by the SDK reshuffles every face, which on air is
   indistinguishable from a bug.
3. Append survivors not already on the wall, in roster order.
4. Truncate to `max_tiles`.

**Manual rules:** return `params.manual` with zeros (`None`) and duplicates
removed. `max_tiles` is an Auto-only control and is not shown in Manual mode;
the resolver still applies it as a bound so the contract is uniform, but with
only nine tile slots the clamp is a no-op in practice. The roster is not
consulted — an operator who
cast a tile keeps it even if that participant's camera is currently off, so a
deliberate assignment is never silently dropped. Manual mode is a casting
decision, not a liveness query.

**Both modes:** duplicate ids collapse to their first occurrence. An empty
result is legal and means an empty wall.

## Wiring

`zoom_supersource` registers a `ZoomEngineClient::add_roster_callback` keyed on
the source instance, mirroring `ZoomSource::on_roster_changed`. On each roster
change, and on each `update`, the source:

1. Reads the current slot list and settings under its mutex, then releases it.
2. Calls `resolve_tile_assignments`.
3. If the result differs from the current slots, hands it to the existing
   `FeedPlan` diff, which already computes register / resubscribe / retire and
   performs the blocking pipe I/O outside `ctx->mutex`.

No new concurrency machinery. The resolver is pure and the plan/apply split it
feeds already exists and is under test.

The callback must be removed in `destroy` before the feeds are retired, so a
roster change cannot arrive against a half-torn-down source.

## Error handling

- Engine not running, or empty roster → empty result → the wall renders neutral.
  Not an error; it is the pre-meeting state.
- A participant assigned in Manual mode who is absent from the roster → still
  subscribed. The engine already reports unresolved subscriptions, and the
  Phase 1 grace handling covers a late joiner.
- Settings holding an id of 0 → treated as an empty slot everywhere.

## Testing

`tests/tile-fill-test.cpp`, a plain `main()` returning 0/1 per repo convention —
no test framework:

1. Auto keeps incumbent order when a newcomer joins (the stability rule).
2. Auto drops a participant whose camera went off and closes the gap.
3. Auto honors `max_tiles`, truncating the newest.
4. Auto honors each exclude slot, and ignores zero-valued excludes.
5. Auto with an empty roster returns empty rather than stale tiles.
6. Manual passes ids through in slot order with `None` holes removed.
7. Manual keeps a cast participant whose camera is off.
8. Duplicate ids collapse in both modes.

## Out of scope

Name-tile text for no-video participants, the subscription-count-against-cap
indicator, drag-to-reorder, and per-tile crop overrides. Phase 2 sync work is
unaffected — this changes only which participants are assigned, never how their
frames are timed.

## Risk

The concurrent-stream cap is unresolved and sits underneath this feature: on
2026-08-09 only 1 of 6 subscribed feeds survived. A picker that makes it easy to
assign nine participants will surface that limit faster and more visibly than
typed IDs did. `max_tiles` defaulting to 9 is a UI ceiling, not a guarantee the
engine can deliver nine live feeds. That work is tracked separately and remains
the real gate on shipping Tiles.
