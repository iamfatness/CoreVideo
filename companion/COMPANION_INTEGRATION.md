# CoreVideo × Bitfocus Companion Integration

CoreVideo exposes two local control APIs that Companion can drive:

| Protocol | Port  | Best for |
|----------|-------|----------|
| TCP JSON | 19870 | Custom module (this module), scripting, full push events |
| UDP OSC  | 19871 | Companion's built-in OSC module, TouchOSC, quick prototyping |

---

## Option A — Custom Companion Module (recommended)

The `companion-module-corevideo-obs/` directory in this repo is a full
Companion v3 module written in TypeScript.

### What it provides

**Actions**
| Action | Description |
|--------|-------------|
| Join Meeting | Join by ID, URL, or passcode |
| Leave Meeting | Leave the current meeting |
| Assign Participant to Output | Route a participant to an OBS source |
| Assign Spotlight Slot to Output | Track Zoom spotlight slot N |
| Assign Screen Share to Output | Route active screen share |
| Cancel Auto-Recovery | Abort a reconnect attempt |
| Refresh Status | Pull fresh state from OBS |

**Feedbacks**
| Feedback | Description |
|----------|-------------|
| Meeting State | Boolean: is the meeting in state X? |
| Meeting State Color (dynamic) | Button color tracks the state automatically |
| Is Active Speaker (by ID) | Lights up when participant ID is active speaker |
| Output Has Participant | Lights up when a source has someone assigned |
| Output Tracking Active Speaker | Lights up when source is in active-speaker mode |
| Auto-Recovery Active | Lights up during reconnect attempts |

**Variables**
| Variable | Value |
|----------|-------|
| `$(corevideo-obs:meeting_state)` | `idle` / `joining` / `in_meeting` / … |
| `$(corevideo-obs:active_speaker_name)` | Display name of current active speaker |
| `$(corevideo-obs:active_speaker_id)` | Numeric user ID |
| `$(corevideo-obs:participant_count)` | Number of participants in roster |
| `$(corevideo-obs:output_N_source)` | OBS source name for output N |
| `$(corevideo-obs:output_N_participant)` | Participant name assigned to output N |
| `$(corevideo-obs:output_N_mode)` | Assignment mode for output N |

### Push events

On connect the module sends `{"cmd": "subscribe_events"}`.  CoreVideo
immediately streams JSON events whenever state changes — no polling needed:

```json
{"event": "meeting_state", "state": "in_meeting"}
{"event": "active_speaker", "user_id": 12345678, "name": "Alex Rivera"}
{"event": "roster_changed"}
{"event": "output_changed"}
```

### Installing during development

```bash
cd companion/companion-module-corevideo-obs
npm install
npm run build
# Then add the dist/ folder as a local module in Companion Developer Mode
```

---

## Option B — Built-in OSC module

Use Companion's built-in **OSC** connection type pointed at `127.0.0.1:19871`.

### Subscribe for push events

On **Connection initialise**, send:

| Address | Type | Value |
|---------|------|-------|
| `/zoom/subscribe` | — | (no args) |

CoreVideo will push events back to the source IP:port it received the
subscription from.  Make sure Companion's OSC **receive port** matches the port
Companion uses to send — or set an explicit receive port and send a custom
subscribe message.

### OSC command reference

#### Inbound (Companion → CoreVideo)

| Address | Args | Description |
|---------|------|-------------|
| `/zoom/subscribe` | — | Register for push events (TTL 5 min, resend to renew) |
| `/zoom/unsubscribe` | — | Cancel push events |
| `/zoom/ping` | — | Keepalive; replies `/zoom/pong` |
| `/zoom/status` | — | Request current meeting state |
| `/zoom/list_participants` | — | Request participant roster |
| `/zoom/list_outputs` | — | Request output assignments |
| `/zoom/join` | `s` meeting_id [`s` passcode [`s` display_name]] | Join meeting |
| `/zoom/leave` | — | Leave meeting |
| `/zoom/assign_output` | `s` source `i` participant_id [`i` active_speaker] | Assign participant |
| `/zoom/assign_output/active_speaker` | `s` source | Route active speaker |
| `/zoom/output/assign_ex` | `s` source `s` mode `i` participant_id `i` spotlight_slot | Extended assign |
| `/zoom/output/audio_mode` | `s` source `s` "mono"\|"stereo" | Set audio mode |
| `/zoom/output/failover` | `s` source `i` failover_participant_id | Set failover |
| `/zoom/isolate_audio` | `s` source `i` 0\|1 | Toggle audio isolation |
| `/zoom/recovery/cancel` | — | Cancel auto-recovery |

#### Outbound push events (CoreVideo → Companion)

| Address | Args | Description |
|---------|------|-------------|
| `/zoom/event/meeting_state` | `s` state | State changed |
| `/zoom/event/active_speaker` | `i` user_id `s` name | Active speaker changed |
| `/zoom/event/roster_changed` | — | Participant joined or left |

#### Status replies (unicast back to requester)

| Address | Args | Description |
|---------|------|-------------|
| `/zoom/status/meeting_state` | `s` state | Reply to `/zoom/status` |
| `/zoom/status/active_speaker` | `i` user_id | Reply to `/zoom/status` |
| `/zoom/participant` | `i` id `s` name `i` has_video `i` is_talking `i` is_muted | One per participant |
| `/zoom/output` | `s` source `i` pid `s` display_name `i` active_speaker `i` isolate | One per output |
| `/zoom/recovery/status` | `i` active `i` attempt `i` max_attempts | Recovery state |

---

## TCP JSON API reference

All requests are newline-delimited JSON sent over a persistent TCP connection
to port 19870.  Include `"token": "…"` in every request if a token is set.

### Subscribing to push events

```json
{"cmd": "subscribe_events"}
```

The connection stays open.  CoreVideo pushes events as they occur:

```json
{"event": "meeting_state", "state": "in_meeting"}
{"event": "active_speaker", "user_id": 16778240, "name": "Sam Chen"}
{"event": "roster_changed"}
```

### All commands

| `cmd` | Key fields | Notes |
|-------|-----------|-------|
| `status` | — | Returns `meeting_state`, `active_speaker_id`, `recovery` |
| `list_participants` | — | Returns `participants` array |
| `list_outputs` | — | Returns `outputs` array |
| `join` | `meeting_id`, `passcode`, `display_name` | Accepts URL or numeric ID |
| `leave` | — | |
| `assign_output` | `source`, `participant_id`, `active_speaker`, `isolate_audio`, `audio_channels` | |
| `assign_output_ex` | `source`, `mode` (participant/active_speaker/spotlight/screen_share), `participant_id`, `spotlight_slot`, `failover_participant_id`, `isolate_audio`, `audio_channels` | Full control |
| `recovery_cancel` | — | Cancel active reconnect |
| `subscribe_events` | — | Keep socket open for push events |
| `oauth_callback` | `url` | Internal OAuth redirect handler |

---

## Remote access

Both servers bind to `127.0.0.1` by default.  To reach CoreVideo from a
Companion instance on a different machine, the OBS host's firewall must allow
the relevant port AND CoreVideo must be configured to accept remote connections
(a setting forthcoming — for now you can use SSH port-forwarding as a safe
workaround).
