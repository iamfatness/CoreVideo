# Support

Current release: **v0.1.44**. Windows 10/11 x64 is the only supported, packaged
platform - see [Platform Support](https://github.com/iamfatness/CoreVideo/blob/main/README.md#platform-support)
before filing a bug against a source build on anything else.

## Documentation

Most configuration and architecture questions are answered on the documentation
site:

**[https://corevideo.io/documentation/](https://corevideo.io/documentation/)**

| Topic | Link |
|---|---|
| Adding the plugin to OBS | [Adding the App](https://corevideo.io/documentation/#adding) |
| Requirements | [Requirements](https://corevideo.io/documentation/#requirements) |
| Configuration reference | [Configuration](https://corevideo.io/documentation/#configuration) |
| Zoom Control dock | [Zoom Control Dock](https://corevideo.io/documentation/#zoom-dock) |
| Assignment modes (participant, active speaker, spotlight, screen share) | [Assignment Modes](https://corevideo.io/documentation/#assignment-modes) |
| Active Speaker Director | [Active Speaker Mode](https://corevideo.io/documentation/#active-speaker) |
| Auto-reconnect | [Auto-Reconnect](https://corevideo.io/documentation/#auto-reconnect) |
| Hardware video acceleration | [Hardware Video Acceleration](https://corevideo.io/documentation/#hw-accel) |
| ISO recording | [Auto ISO Recording](https://corevideo.io/documentation/#iso-recording) |
| TCP control API | [TCP Control API](https://corevideo.io/documentation/#control-api) |
| OSC control API | [OSC Control API](https://corevideo.io/documentation/#osc-api) |
| Sources, talkback, audio routing | [Plugin guide](https://corevideo.io/core-plugin/) |

---

## What things are called

CoreVideo's OBS **sources** are named `CoreVideo ...`, but its **docks and Tools
menu entries** are still named `Zoom ...`. Both belong to this plugin. Searching
the Tools menu for "CoreVideo" finds nothing.

| Docks (View -> Docks, and Tools) | Sources (Sources -> Add) |
|---|---|
| Zoom Control | CoreVideo Participant |
| Zoom Output Manager | CoreVideo Active Speaker |
| Zoom Diagnostics | CoreVideo Screen Share |
| Zoom ISO Recorder | CoreVideo Tiles |
| Zoom Talkback | CoreVideo Participant Audio |
| (Tools only) Zoom Plugin Settings | CoreVideo Active Speaker Audio |
| | CoreVideo Audience Audio (legacy) |
| | Zoom Interpretation Audio |

---

## Collecting logs

### The support bundle - do this first

Open the **Zoom Diagnostics** dock (or **Tools -> Zoom Diagnostics**) and click
**Create Support Bundle**. This is the single most useful thing you can attach
to a bug report, and it is what the GitHub issue template asks for.

The bundle is written to:

```
%APPDATA%\obs-studio\plugin_config\obs-zoom-plugin\support-bundles\CoreVideo-support-<yyyyMMdd-HHmmss>\
```

On Windows a `.zip` of that folder is created beside it, using PowerShell. The
dialog tells you both paths when it finishes. If PowerShell is unavailable, or
you are not on Windows, only the folder is written and you zip it yourself.

It contains `summary.json`, `summary.txt`, `engine-status.json`,
`settings-redacted.json`, `runtime-manifest.json`, `iso-recorder.json`, and
`obs-latest.log` - a redacted excerpt of the last 500 lines of the newest OBS
log. Inside are the plugin version, meeting and engine state, the per-output
table (assignment, requested/negotiated/observed resolution, fps, health), the
participant roster, the last engine debug events, ISO recorder status, a
manifest of the runtime files that are supposed to be installed, and a
machine-readable `failure_classifications` list - values such as
`missing_runtime`, `auth_not_ready`, `no_video_frames`, `stale_feed`,
`low_quality_feed`, `iso_encoder_error`.

**What is redacted:** OAuth access and refresh tokens, ZAK/JWT values, client
secrets, passcodes, the control-server token, and `Authorization:` headers, in
the JSON and in the log excerpt. Secrets are reduced to a length and their last
four characters, or to a presence flag.

**What is not redacted:** meeting and participant display names, participant
IDs, and ISO recording file paths. Look through the bundle before attaching it
to a public issue if any of that is sensitive.

### The OBS log

CoreVideo has no log file of its own. Everything - plugin and engine alike -
goes into the OBS log, prefixed `[obs-zoom-plugin]`. Engine lines arrive as
`[obs-zoom-plugin] Zoom engine debug: ...`.

In OBS: **Help -> Log Files -> Upload Current Log File**, then paste the link.
The files themselves are under `%APPDATA%\obs-studio\logs`.

The version is logged once at load, so check that first - a stale DLL is a
common cause of "the fix didn't work":

```
[obs-zoom-plugin] Loading plugin v0.1.44
```

### Verbose engine logging

High-frequency engine events (per-frame and per-buffer stages) are suppressed by
default - they were producing a 27 MB log in a 90-minute meeting. To turn them
on, set the environment variable `CV_ZOOM_VERBOSE_LOG` to any value other than
`0` before starting OBS:

```
set CV_ZOOM_VERBOSE_LOG=1
"C:\Program Files\obs-studio\bin\64bit\obs64.exe"
```

Expect a very large log. Turn it off again afterwards. Note these events are
captured in the diagnostics ring buffer - and so in the support bundle - even
when they are not in the OBS log, so try the support bundle before resorting to
this.

### ISO recording

Each ISO session writes an `<recording>.ffmpeg.log` sidecar next to its output
files. If an ISO file is missing or truncated, that log holds FFmpeg's own
account of why.

---

## Reporting Bugs

Open an issue at **[github.com/iamfatness/CoreVideo/issues](https://github.com/iamfatness/CoreVideo/issues)**.
The bug report template walks through the same list:

- **CoreVideo version** - the `[obs-zoom-plugin] Loading plugin v...` line
- **OBS Studio version**
- **Windows version**
- **A support bundle** from the Zoom Diagnostics dock
- **Steps to reproduce**, and expected versus actual behaviour
- Whether the meeting used **breakout rooms**, a **waiting room**, or was joined
  as a **Webinar / Zoom Events** session - each has its own failure modes

For security vulnerabilities, **do not open a public issue**. See
[SECURITY.md](https://github.com/iamfatness/CoreVideo/blob/main/SECURITY.md).

---

## Common Issues

### Everything worked yesterday and now audio breaks up, or the engine will not start

**Cause:** an orphaned `ZoomObsEngine.exe` from a previous OBS session. If OBS
exited while the Zoom SDK was wedged, the engine can survive as an orphan -
still in the meeting, still writing into shared memory under the names the next
session wants to use. The new session then shares its audio transport with a
ghost writer, which suppresses its audio wakeups (audio breaks up on every
source), corrupts video regions, and holds the SDK singleton so the first engine
start of the day fails.

**Fix:** since v0.1.41 every engine start sweeps stale engine processes first and
logs what it removed, so this should not happen. If it does, the plugin reports
a shared-memory name collision as a visible error naming the cure: close OBS, end
any `ZoomObsEngine.exe` in Task Manager, and start again.

---

### The plugin cannot launch or reach the engine

**Cause:** a partial install. CoreVideo is two binaries and they must match -
`obs-zoom-plugin.dll` **and** `zoom-runtime\ZoomObsEngine.exe`. Copying only the
DLL is the most common self-inflicted failure with this plugin; roughly half the
fixes in any release are engine-side.

**Fix:**
1. Reinstall from the official installer rather than copying files.
2. Confirm the layout under your OBS install:
   - `obs-plugins\64bit\obs-zoom-plugin.dll`
   - `obs-plugins\64bit\zoom-runtime\ZoomObsEngine.exe`
   - `obs-plugins\64bit\zoom-runtime\sdk.dll` and the rest of the Zoom SDK runtime
   - `obs-plugins\64bit\plugins\platforms\qwindows.dll`
   - `obs-plugins\64bit\plugins\tls\qschannelbackend.dll`
3. The support bundle's `runtime-manifest.json` checks exactly these files and
   says which are missing - it is faster than checking by hand.
4. If you are upgrading from a build older than v0.1.30, delete any loose FFmpeg
   DLLs left in `obs-plugins\64bit` by the old layout. The plugin logs a warning
   naming them.

---

### The join sits on "joining" and never finishes

**Two known causes, and they look identical from the dock.**

**A waiting room.** This is normal and CoreVideo now waits it out - as of
v0.1.44 the two-minute join watchdog holds its window open for as long as a
legitimate waiting-room or waiting-for-host state lasts, rather than counting it
as no progress and leaving. Before v0.1.44 a host who took longer than two
minutes to admit got an auto-leave and a Failed attempt; if you are on an older
build, upgrade.

**Your own Zoom account is already hosting a meeting somewhere else.** Joining
with a ZAK for an account that is hosting elsewhere - your own client in your
own PMI, which is the ordinary way anyone tests - does not fail the join. The
SDK silently asks whether to end the other meeting, and if nobody answers,
`Join()` never resolves and no failure is ever reported. CoreVideo now answers
"no" (it will never end your live show to join a meeting) and fails the join
loudly instead, with reason `account_busy_elsewhere` and code `909001`. If you
see that, leave the other meeting or use a different account.

Note the join watchdog is armed by the dock's **Join** button only. A join
issued through the TCP or OSC control API is not watched.

---

### OAuth sign-in or join authentication fails

**Fix:**
1. Sign out and sign back in from **Tools -> Zoom Plugin Settings** - the
   **Sign in with Zoom** and **Sign out** buttons are there, not on the Zoom
   Control dock.
2. Leave the join-token dropdown on the Zoom Control dock set to **Zoom
   sign-in**. `User ZAK` and `App privilege token` are for tokens Zoom support
   has given you specifically; a stale value there will fail every join.
3. Confirm the broker page loads: `https://corevideo.iamfatness.us/oauth/start`.
   That is the host compiled into published builds; the site itself is
   corevideo.io, and both names route to the same worker.
4. Check the OBS log for `[obs-zoom-plugin]` OAuth, ZAK, or Meeting SDK auth
   errors.

If you run your own Marketplace app rather than the published one, it needs
Public Client OAuth with PKCE, Meeting SDK / Embed enabled in the same
environment, and `/oauth/callback` on your broker host registered as the
redirect URL. See
[OAuth PKCE](https://corevideo.io/documentation/#flow-oauth).

---

### Every video source goes black after a breakout room

**Cause:** moving into or out of a breakout room does not disconnect you, so the
engine believed raw recording was still running - while Zoom had quietly revoked
the permission. Every source then failed to subscribe with
`SDKERR_NO_PERMISSION` and stayed black.

**Fix:** fixed in v0.1.40 - re-entry now re-requests the recording privilege the
same way the initial join does. On an older build, stop and start raw media from
the Zoom Control dock. Note also that talkback reaches only the breakout room
the engine is in.

---

### Blank / black video from a participant

**Cause:** the participant's camera is off, or the subscription was dropped.

**Fix:**
1. Confirm the participant has their camera enabled in Zoom. Someone with their
   camera off cannot be routed to an output or a tile at all - the Output
   Manager's **Hide participants without video** toggle filters them out of the
   pickers, and never hides a participant who is already assigned.
2. Set **On video loss** to **Hold last frame** in the source's properties so a
   brief drop does not go black.
3. Click **Refresh participant list** in the source's properties and re-select
   the participant.
4. Check the Zoom Diagnostics dock for requested versus observed resolution,
   frame age, and stale/quality retry counters. That is the fastest way to tell
   "waiting for a first frame" from "receiving a lower feed than requested"
   from "being resubscribed by recovery".

---

### No audio, or audio that breaks up

**First, know which of the two audio paths you are on** - they have different
guarantees and no code connects them.

- The **dedicated** path is the audio-only sources: **CoreVideo Participant
  Audio**, **CoreVideo Active Speaker Audio**. These drain an 8-slot ring, stamp
  timestamps from a master clock derived from the sample count, count what they
  lose, and honour the audio delay trim. **Route these to program for anything
  that matters.**
- The **embedded** path is the audio track published alongside a CoreVideo video
  source's picture. It reads only the newest buffer, is stamped at arrival, and
  has no loss accounting.

**Fix:**
1. For a video source, set **Audio Channels** to **Mono** or **Stereo** (not
   None) in source properties, and confirm the participant is unmuted in Zoom.
2. Check the OBS audio mixer - the track may be muted or monitoring-only.
3. If you are using **Isolate selected participant's audio**, confirm the right
   participant is selected.
4. Gaps are often Zoom, not CoreVideo: Zoom only calls audio back for a
   participant who is currently making sound. A silent stretch produces no
   buffers at all. CoreVideo fills the gap and ramps in the resumption so it does
   not click, but it cannot invent speech that was never sent.
5. Continuous break-up on every source at once points at the ghost-engine case
   above, or at the machine not keeping up - `list_audio_sources` over the TCP
   API reports `overrun_slots` per source, which should stay at `0`, and
   `audio_latency_us`, which should be sub-millisecond.

---

### Audio is ahead of the picture

Video is the slower path in any software production chain, so audio arrives
early and needs delaying to line back up. There is one control per audio path:

- **Tools -> Zoom Plugin Settings -> Audio -> Audio delay (dedicated sources)** -
  a single global trim, 0-500 ms, for every dedicated CoreVideo Audio source. It
  takes effect on the next buffer, including on sources already running.
- The Output Manager's per-row **Delay (embedded)** trims one video source's own
  embedded track, and **A/V Offset (embedded)** is the measured number to trim it
  against - positive means audio is early by that many milliseconds.

Trim off air. Lowering a delay pushes the timestamp backward once and briefly
glitches that source.

---

### Talkback: the key says live but nobody hears me

The Zoom Talkback dock is new, and the failure modes are all silent in Zoom's
own API.

- **The banner reads "ON AIR - BOT MUTED".** Zoom refused to unmute CoreVideo's
  microphone, and sends are accepted while delivering nothing. Ask the host to
  unmute CoreVideo.
- **A cell reads "not in channel".** They have a channel but are not in it -
  most often because they are in a different breakout room. Talkback reaches only
  the room the engine is in.
- **A cell reads "no talkback".** Their Zoom client reported no talkback support,
  or the channel budget could not cover them.
- **A cell reads "no channel" or "assigning...".** Nobody is reachable until
  channels are assigned. Open **Edit talent**, tick who you may need to talk to,
  and press **Assign channels**. Zoom allows 16 channels and 10 people per
  channel; the plan report names anyone the budget could not cover.
- **Assigning takes a while.** Zoom rate-limits channel and invite calls, so
  CoreVideo paces them at one every 600 ms. A large talent list can take around
  20 seconds to fully provision. That cost is paid once, at assignment, and never
  at key time.
- **Nothing is keyable until you choose a talk source.** Pick the OBS audio
  source you talk through in the dock's source combo, and check the line beneath
  it: it tells you whether that source is also going out on a program track,
  which the audience would hear.

---

### Auto-reconnect triggers but never succeeds

**Cause:** the underlying error is permanent.

**Fix:**
1. Check the OBS log for the line saying why recovery stopped - for example
   `License error - reconnect not attempted`, `Auth failure - reconnect
   suppressed by policy`, `Host ended meeting - not reconnecting`, or
   `Engine crash - reconnect suppressed by policy`. Every path that declines to
   reconnect logs its own reason.
2. On a license error, see the raw-data entitlement note below.
3. On an auth failure, sign out and back in from **Tools -> Zoom Plugin
   Settings**.
4. Click **Cancel Recovery** in the Zoom Control dock, fix the root cause, and
   join again. Cancel stops the reconnect timers, stops the engine, and clears
   the stored session so the retry loop cannot restart itself.

---

### Raw-data permission or stream-count error at runtime

**Cause:** raw data access comes with Meeting SDK apps, but the signed-in account
and app entitlements still determine negotiated quality, bandwidth, and stream
count. Standard accounts are typically limited to a 30 Mbps incoming video
budget; Enhanced Media / HBM can raise that to roughly 100 Mbps. Developers may
also need an app-level entitlement flag to test more than a small number of
concurrent raw streams.

**Fix:** verify the Meeting SDK app is approved or beta-enabled for the account
joining the meeting, then confirm the expected bandwidth and developer/app
entitlements with Zoom. Treat Enhanced Media / HBM as a production quality and
bandwidth tier, not as a hard prerequisite for raw data.

---

### ISO recording produces nothing, or a short file

1. `ffmpeg` must be on `PATH`, or given explicitly as `ffmpeg_path`. The ISO
   Recorder dock has a test button for it.
2. Read the `.ffmpeg.log` beside the output files.
3. The encoder demotion chain is NVENC -> QSV -> AMF -> libx264. If your machine
   has no working QSV or AMF runtime, a source demoted off NVENC now walks the
   chain all the way to libx264 rather than getting stuck (fixed in v0.1.42).
4. Recording is blocked below 2 GB free on the output volume and warns below
   10 GB.
5. ISO durations that did not match real elapsed time, and WAVs that were double
   or half length, were fixed in v0.1.42 and v0.1.43. If you see either, check
   your version first.

---

### TCP or OSC control API not responding

1. Confirm the ports in **Tools -> Zoom Plugin Settings** match your client.
   Defaults are TCP `19870` and UDP OSC `19871`.
2. Both servers bind to `127.0.0.1` only. A control surface on another machine
   cannot reach them; put a local bridge on the OBS machine.
3. Look for `TCP control server unavailable` or `OSC server unavailable` in the
   OBS log - another process may already own the port. Neither failure stops the
   plugin loading.
4. If you set a token in settings, every TCP command line must carry a `"token"`
   field. With no token set, authentication is disabled and the plugin logs a
   warning saying so - the OSC server has no authentication at all either way.
   Both are loopback-only, but any local process can drive them.

---

## Known limitations

- **v0.1.43 was built and verified but never published.** Anyone tracking
  releases goes from v0.1.42 straight to v0.1.44, which contains both.
- **The installer is not code-signed**, so Windows SmartScreen flags it on first
  run.
- **The embedded audio track** on a video source has no loss accounting and is
  stamped at arrival. Use the dedicated CoreVideo Audio sources for critical
  audio.
- **The intercom / talkback dock is newer than v0.1.44** and is not in a tagged
  release yet. It is on `main` and ships next. Talkback has no OSC addresses,
  no Companion actions, and no OBS hotkey yet - the dock and the TCP control API
  are the only keying surfaces.
- **macOS and Linux are source-build only.** There are no official packages, no
  QA, and no supported upgrade path.
- **`COREVIDEO_TALKBACK_LAYOUT_TEST`** is a developer instrument that fills the
  Talkback dock with a fake cast and refuses every engine call. Never set it on a
  show machine.

---

## Zoom Bandwidth and Enhanced Media / HBM

CoreVideo does not require Enhanced Media / HBM simply to access Meeting SDK raw
data. Quality and concurrency are still bounded by Zoom account and app
entitlements:

- Standard accounts commonly operate within a 30 Mbps incoming video envelope.
- Enhanced Media / HBM can raise the incoming video envelope to roughly 100 Mbps.
- Standard 1080p feeds are typically about 4-6 Mbps each.
- 100 Mbps can support roughly 16 standard 1080p feeds, or about 8 high-bitrate /
  60 fps feeds.
- Developers may need a separate app entitlement flag for testing more than a
  small number of raw streams; end users should not need that developer flag.

---

## Feature Requests

Feature requests are welcome as GitHub issues. Search existing issues first, and
label your request `enhancement`. What is already planned is in the
[Roadmap](https://github.com/iamfatness/CoreVideo/blob/main/docs/ROADMAP.md).
