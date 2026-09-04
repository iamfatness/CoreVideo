# CoreVideo Operator Quickstart

This guide is for an operator using a published CoreVideo installer. It avoids
source-build and Marketplace setup details unless they affect day-to-day use.

## Install

1. Download the latest Windows installer from GitHub Releases.
2. Close OBS Studio before running the installer.
3. Run `CoreVideo-Setup-vX.Y.Z.exe`.
4. Start OBS Studio.
5. Confirm these OBS docks/tools are available:
   - **Zoom Control**
   - **Zoom Output Manager**
   - **Zoom Diagnostics**
   - **Zoom ISO Recorder**
   - **Zoom Talkback**

Each has a matching entry under **Tools**, alongside **Zoom Plugin Settings**.

Published installers include the OBS plugin, `ZoomObsEngine`, Zoom SDK runtime,
Qt runtime, TLS plugins, OAuth callback helper, and locale files. End users do
not need to download the Zoom SDK or enter Zoom app credentials.

## Sign In

1. Open **Tools > Zoom Plugin Settings**.
2. Click **Sign in with Zoom**. This button is in the settings dialog, not on the
   Zoom Control dock.
3. Approve CoreVideo in the browser.
4. Return to OBS and confirm the dialog's status line shows you are signed in.
   **Sign out** is beside it if you ever need to start a fresh flow.

CoreVideo uses the published app's Public Client OAuth and Meeting SDK public
app key. There should be no prompt for a client secret in a production build.

## Join A Meeting

1. Enter the meeting ID, or paste a full Zoom join URL.
2. Enter the passcode if needed.
3. Choose the join display name.
4. Leave the join-token dropdown on **Zoom sign-in** unless Zoom support has
   given you a ZAK or an app privilege token to paste.
5. Tick **Join as Webinar / Zoom Events** for a webinar.
6. Click **Join**, then **Start Engine** once you are in.
7. Use the visible Zoom Meeting SDK window for waiting room admit, self audio,
   self video, and normal in-meeting controls. Joining early and waiting to be
   admitted is fine - CoreVideo holds its join window open for the length of a
   legitimate waiting-room wait rather than giving up on it.

If join fails, open **Zoom Diagnostics** and export a support bundle before
changing settings. The bundle redacts tokens and includes OBS/engine/ISO status.

## Assign Outputs

1. Open **Zoom Output Manager**.
2. For each CoreVideo participant source, choose an assignment mode:
   - **Participant** for a fixed Zoom participant.
   - **Active Speaker** for the directed speaker feed.
   - **Spotlight Slot 1-8** for ZoomISO-style fixed stage slots.
   - **Screen share** for the active meeting share.
3. Request 1080p for production sources when the Zoom account and meeting
   entitlements support it.
4. Watch the health markers:
   - Resolution/FPS show what Zoom is actually delivering.
   - A warning marker means the observed feed does not match the requested
     output or is stale.
   - Active-speaker, spotlight, and screen-share assignments show dedicated
     warnings when no eligible routed source is available.

CoreVideo keeps OBS sources at a stable canvas size and lets OBS scale lower
quality feeds instead of changing source geometry during a meeting.

## Tiles Wall

Use **CoreVideo Tiles** when you want everyone on screen at once as a gallery,
in a single OBS source that relays itself as people come and go.

1. Add **CoreVideo Tiles** from **Sources -> Add**.
2. Leave **Fill mode** on `Auto - everyone with video` to fill the wall with
   whoever has video, or pick `Manual - choose per tile` to place people
   yourself.
3. Set **Maximum tiles** to the largest wall you want, and list anyone who
   should never appear under **Never show** — a stage camera or a recording bot.
4. Style it: **Tile shape**, **Gap between tiles**, **Margin around the wall**,
   **Background colour** and optional **Background source**, then borders and
   glow if the show wants them.
5. For per-person faders and ISO tracks, set **Participant audio scene or
   group** to a scene or group, and add that scene or group to every scene so
   audio does not come and go as you cut. Leave it blank if you do not want the
   wall creating audio sources.
6. Optionally turn on **Animate layout changes** so the wall eases when people
   join and leave instead of jumping on one frame. It is off by default. Set it
   before you go live rather than mid-show, and give it a look on program
   first — a busy roster reflows more often than you might expect.

Tiles are always filled, never letterboxed: a tile narrower than the camera
feeding it crops the sides. If you run a second wall, leave its audio field
blank — only one wall should own participant audio.

## Active Speaker

Use the **CoreVideo Active Speaker** OBS source when you want one clean output
that follows the current directed speaker.

1. Add or select **CoreVideo Active Speaker**.
2. Configure sensitivity and hold time in source properties or Zoom Control.
3. Use exclusion slots for fixed host/question-reader workflows.
4. Use manual take/release when a producer needs to override automatic switching.

The director is CoreVideo's own speaker-follow logic. It is not the same thing
as Zoom's active-speaker video feed.

## Audio

Route the dedicated **CoreVideo Participant Audio** and **CoreVideo Active
Speaker Audio** sources to program rather than relying on a video source's own
embedded audio track. The dedicated path drains the engine's ring in order,
stamps from a master clock, and counts what it loses; the embedded track reads
only the newest buffer and accounts for nothing.

Audio arrives ahead of picture, so trim it back - **Tools > Zoom Plugin Settings
> Audio > Audio delay (dedicated sources)** for the dedicated sources, and the
Output Manager's per-row **Delay (embedded)** for one video source's own track,
against the **A/V Offset (embedded)** column beside it. Trim off air: lowering a
delay pushes the timestamp backward once and glitches that source.

## Talkback

**Newer than v0.1.44 - on `main`, not in the v0.1.44 installer.**

Open **Zoom Talkback** to talk privately to talent over Zoom's own talkback
channels.

1. Choose the OBS audio source you talk through in the dock's source combo. Use
   a dedicated source with every program track unchecked in Advanced Audio
   Properties, and read the line under the combo - it tells you whether the
   audience would hear it.
2. Press **Edit talent**, tick everyone you may need to talk to, press **Assign
   channels**, then **Done**. Channels are created now so a key press has only to
   open the microphone. Zoom rate-limits this, so a large list can take around
   twenty seconds. Do it before the show, not during it.
3. Read the plan report. Zoom allows 16 channels and 10 people per channel;
   anyone the budget could not cover is named.
4. Hold a cell to talk, or tick **Latch** for press-on/press-off. **All talent**
   is the full-width cell on top.
5. Trust the banner, not the button. `ON AIR` means the engine confirmed it;
   `ON AIR - BOT MUTED` means Zoom will not let CoreVideo unmute and nobody hears
   you - ask the host to unmute CoreVideo.

Talkback reaches only the breakout room the engine is in. A cell reading `not in
channel` usually means the talent is in a different room.

## ISO Recording

1. Open **Zoom ISO Recorder**.
2. Choose an output folder.
3. Choose the video encoder:
   - `libx264` is the safest CPU fallback.
   - `h264_nvenc`, `h264_qsv`, and `h264_amf` reduce CPU load when supported by
     the installed FFmpeg build and hardware.
4. Enable **Record program** if CoreVideo should also start OBS program
   recording.
5. Click **Start ISO Recording**.

ISO recording follows assigned outputs. It records assigned participant video
to MP4 and matching audio to WAV. The panel shows requested encoder, actual
encoder, fallback state, active sessions, and file paths.

## Diagnostics And Support

Use **Zoom Diagnostics** during every production test. Export a support bundle
when you see:

- Join/auth errors.
- Engine crash or reconnect loop.
- Stale frames.
- Resolution stuck below the requested quality.
- ISO encoder failures.
- OBS close/reopen dock issues.

The bundle includes redacted settings, recent OBS log excerpts, engine status,
output health, active speaker status, screen-share status, and ISO recorder
status.

## Known Scope

The OBS plugin is the production surface today. Sidecar is tracked separately
and should not be presented as the production scene/look designer until the
Sidecar roadmap issues are complete.
