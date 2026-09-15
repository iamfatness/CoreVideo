# ISO feed selection

Development build: `v0.1.47-dev.3`. Includes participant A/V recording and the
pending Tiles frame-handoff fix. Not installed while the operator is in a meeting.

The ISO panel now lists output feeds with checkboxes, defaulting to none.
Selections are stored by source UUID, survive live routing refreshes, and are
locked for the recording run. Unrouted/unsupported entries cannot be newly
selected; a saved choice is retained if its route temporarily disappears.
Start requires at least one selected routed feed. Disk and encoder estimates
count only selected feeds, deduplicating fixed participant routes.

The recorder validates selection independently of the UI and filters both
audio and video callbacks. Starting no longer pre-opens writers for configured
participant IDs. Files open only on media delivery from a selected route.
Once opened, normal gap handling and one-file-per-participant ownership remain.

TCP `iso_recording_start` accepts `source_uuids`, an array of output UUIDs;
omission uses saved panel choices, and explicit empty selection is rejected.
OSC start uses saved choices. Neither API falls back to recording every source.
Status exposes `selected_source_uuids`; the panel shows an armed/waiting state
when recording is enabled but no selected feed has delivered media.

Windows full build succeeded and all 70 CTest regressions passed. The new
selection-policy regression covers selected/unselected, unrouted, unsupported,
empty selection, and empty UUID cases. Live UI and recording acceptance remain
pending the operator's meeting ending and installation: select two routed
feeds, leave others unchecked, verify only selected participants create files,
and confirm offline/unrouted feeds create none. Check saved choices after OBS
restart and a routing refresh.
