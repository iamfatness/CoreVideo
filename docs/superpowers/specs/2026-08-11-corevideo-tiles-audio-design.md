# CoreVideo Tiles — Per-Participant Audio

**Date:** 2026-08-11
**Status:** Approved by the owner 2026-08-11 16:29
**Builds on:** `2026-08-10-corevideo-tiles-v2-design.md`,
`2026-08-11-corevideo-tiles-gallery-styling-design.md`

## Problem

The Tiles wall is video-only. Audio comes from three separate source kinds
(Participant, Active Speaker, Audience) that the operator creates and assigns by
hand, one per person. Casting a nine-person wall therefore means building nine
audio sources a second time, and keeping them matched as the wall reflows.

The owner wants both **live per-person faders** and **ISO stems for post**, and
raised a specific worry: that switching between a person and the wall would
produce "weird audio".

## The constraint that shapes everything

In OBS a source has **one** audio stream — one fader, one set of track
assignments. A Tiles source can therefore never expose nine faders, however it
is built. Per-person control requires per-person sources. The only real design
question is who creates and maintains them.

## Why the swap worry is real, and how the topology answers it

A source is audible when it is in the active program scene. If the wall carried
the meeting mix *and* per-person sources also fed program, any scene containing
both would play each voice twice — and during a transition both scenes are
briefly active, so every cut would swell. That is how OBS scene audio works; it
is not something the plugin can paper over.

**So audio stops being scene-scoped.** The participant audio sources live in a
group present in *every* scene. Cutting between video scenes then never touches
audio: there is nothing to swap, so nothing to glitch. Each voice is carried
exactly once, keeps a fader, and can be routed to its own track.

**The wall stays silent by design.** Making it audible would reintroduce
doubling the moment both it and a person were on air. Under this topology there
is no person→wall audio transition at all, because the audio never came from the
wall.

## Decisions

| Question | Decision |
|---|---|
| Who creates the audio sources | **The Tiles source**, auto-created and kept in sync with its assignments |
| What gets created | One `zoom_participant_audio_source` per participant, keyed by its existing `participant_id` setting |
| Where they live | A **group the operator nominates**, which they place in every scene |
| Whether it runs at all | **Opt-in, off by default.** See below |
| Wall audio | **Silent.** Never carries the mix |
| When someone leaves the wall | **Mute and keep**, not delete — *recommended default, not an explicit owner choice* |
| Track assignment | Everyone on track 1 (program); tracks 2-6 carry five ISO stems; the rest are program-only, logged once — *recommended default, not an explicit owner choice* |

Both rows marked *recommended* were proposed by the assistant and accepted with
the design as a whole. They are the two most likely things the owner will want
to change once they have used it.

## Opt-in, and why that is not timidity

The feature is **off by default**. Nothing is created until the operator names
a group, which is also the switch that turns it on.

This matters because the plugin ships to existing scene collections. A feature
that defaulted on would, on the next plugin update, silently spawn audio sources
in the collection of every operator who never asked for it — during a show, in
Auto mode, as the wall reflowed. The default state of a feature that writes to
someone else's scene collection is off.

## One owner per participant

Two Tiles sources can show the same person — a main wall and a smaller one, say.
If both auto-created audio for that participant, the voice would be carried
twice and the mix would double: precisely the artefact this whole topology
exists to prevent.

So an auto-created audio source records **which Tiles source owns it**. A Tiles
source creates audio only for participants no other Tiles source already owns,
and only ever mutes, retracks, or cleans up sources it owns itself. If the
owning Tiles source is deleted, its sources are left in place and unowned; the
next Tiles source to need that participant adopts them rather than making
duplicates.

## The guardrail

The plugin is now creating and destroying sources in the operator's scene
collection. That is the risk to design against, and the failure mode that
matters is not a missing audio source — it is the plugin quietly removing or
re-pointing something the operator made by hand, mid-show.

Rules:

- The plugin touches **only sources it created**, identified by a marker stored
  in the source's own settings — never by name, which the operator can change.
- If a name it wants is already taken by a source it does not own, it **defers
  and logs** rather than overwriting or renaming.
- It never removes a source the operator created, never edits their filters, and
  never changes their track assignments.
- Deleting an auto-created source is a **manual** action; the plugin will not do
  it on reflow.

## Removal policy

When a participant leaves the wall, their audio source is **muted, not
removed**. A source vanishing mid-show takes its fader, its filters and any
operator tuning with it, and in Auto mode the wall reflows constantly — so
deletion would be both frequent and lossy. Muting is reversible and cheap.

Cleanup of long-dead sources is an explicit operator action, not automatic.

## Track assignment

Every created source joins **track 1**, the program mix — that is what gives
the operator a live fader for each person. Tracks **2 through 6** then carry one
ISO stem each, allocated in tile order.

So the real ceiling is **five stems**, not six: track 1 is spent on the live
mix. A wall of six or more people cannot give everyone their own stem.

When the stem tracks run out, the remaining participants stay on track 1 alone
and the plugin logs once, naming how many exceeded the ceiling. It degrades
honestly rather than silently dropping stems.

## Error handling

- Nominated group missing or deleted → log once, create nothing, leave existing
  sources alone. Not an error state; the operator may not have made it yet.
- A participant with no audio available → the source is created and stays
  silent; that is indistinguishable from someone who is muted.
- Engine not running → no sources created until it is; the existing roster
  callback already drives the retry.
- Name collision with an operator-owned source → defer and log, never overwrite.
- Participant already owned by another Tiles source → skip; no duplicate, no
  doubling.
- Owning Tiles source deleted → its sources stay, unowned and adoptable.

## Testing

Pure and testable, and this is most of the risk:

- The reconciliation between the wall's assignment list and the set of
  auto-created sources — what to create, what to mute, what to leave alone.
- Ownership arbitration: same participant on two walls yields one source, and a
  deleted owner's sources are adopted rather than duplicated.
- Track allocation, including the six-track ceiling.

All three are decision logic with no OBS dependency, and all three are where a
bug would double someone's audio or damage a scene collection.

Not unit-testable, and honest about it: everything touching `obs_source_create`,
group membership, and mixer routing. Those need the rig, and the guardrail above
is the reason they need it carefully — a bug here damages the operator's scene
collection rather than just rendering wrongly.

## Out of scope

Audio filters on created sources, per-participant monitoring settings, automatic
gain or ducking, stem recording configuration beyond track assignment, and any
change to the existing three manual audio source kinds — which continue to work
exactly as they do today for operators who prefer to build audio by hand.
