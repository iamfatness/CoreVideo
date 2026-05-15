# Support

## Documentation

Before opening an issue, check the full documentation site — most configuration questions and architecture details are covered there:

📖 **[https://iamfatness.github.io/CoreVideo/](https://iamfatness.github.io/CoreVideo/)**

| Topic | Link |
|---|---|
| Installation & build | [Installation](https://iamfatness.github.io/CoreVideo/#installation) |
| Configuration | [Configuration](https://iamfatness.github.io/CoreVideo/#configuration) |
| Zoom Control Dock | [Zoom Control Dock](https://iamfatness.github.io/CoreVideo/#zoom-dock) |
| Assignment modes (Spotlight, Active Speaker, Screen Share) | [Assignment Modes](https://iamfatness.github.io/CoreVideo/#assignment-modes) |
| Active speaker debounce | [Active Speaker Mode](https://iamfatness.github.io/CoreVideo/#active-speaker) |
| Auto-reconnect | [Auto-Reconnect](https://iamfatness.github.io/CoreVideo/#auto-reconnect) |
| Hardware video acceleration | [Hardware Video Acceleration](https://iamfatness.github.io/CoreVideo/#hw-accel) |
| TCP control API | [TCP Control API](https://iamfatness.github.io/CoreVideo/#control-api) |
| OSC control API | [OSC Control API](https://iamfatness.github.io/CoreVideo/#osc-api) |
| IPC protocol reference | [IPC Protocol](https://iamfatness.github.io/CoreVideo/#ipc-protocol) |

---

## Reporting Bugs

Open an issue at **[github.com/iamfatness/CoreVideo/issues](https://github.com/iamfatness/CoreVideo/issues)**.

Please include:

- **CoreVideo version** (shown in OBS log: `[obs-zoom-plugin] Loading plugin v…`)
- **OBS Studio version**
- **Operating system** (Windows 10/11, macOS version, Linux distro)
- **Zoom Meeting SDK version** (5.17.x or 7.x)
- **Steps to reproduce** the issue
- **OBS log file** — in OBS go to Help → Log Files → Upload Current Log File and paste the link
- **Expected vs. actual behaviour**

---

## Common Issues

### Raw-data permission error at runtime

**Cause:** The Zoom account joining the meeting does not have a Zoom Enhanced Media License.

**Fix:** Contact your Zoom account representative to verify your Enhanced Media License entitlement. This is a Zoom account requirement — not a plugin bug.

---

### Engine fails to start / IPC connection timeout

**Cause:** `ZoomObsEngine` could not be launched or located.

**Fix:**
1. Confirm `ZoomObsEngine` (`.exe` on Windows) is in the same directory as the plugin.
2. On macOS, remove quarantine: `xattr -d com.apple.quarantine ZoomObsEngine`
3. On Linux, ensure execute permission: `chmod +x ZoomObsEngine`
4. Check the OBS log for `[obs-zoom-plugin]` launch failure messages.

---

### Blank / black video from a participant

**Cause:** Participant camera is off, or the subscription was dropped.

**Fix:**
1. Confirm the participant has their camera enabled in Zoom.
2. Set **On video loss** → **Hold last frame** in source properties to avoid going black on brief drops.
3. Click **Refresh** in the participant list and re-subscribe.

---

### No audio from a participant

**Cause:** Incorrect audio mode or participant is muted.

**Fix:**
1. Set **Audio Channels** to **Mono** or **Stereo** (not None) in source properties.
2. Confirm the participant is unmuted in Zoom.
3. If using **Isolate Audio**, confirm the correct participant ID is selected.
4. Check the OBS audio mixer — the track may be muted or set to Monitor Only.

---

### Auto-reconnect triggers but never succeeds

**Cause:** The underlying error (auth failure, license issue, network) is permanent.

**Fix:**
1. Check the OBS log for the `RecoveryReason` and error codes.
2. If the reason is `LicenseError`, see the raw-data permission issue above.
3. If the reason is `AuthFailure`, check your SDK key/secret in **Tools → Zoom Plugin Settings**.
4. Click **Cancel Recovery** in the Zoom Control dock, resolve the root cause, then re-join manually.

---

### TCP or OSC control API not responding

**Fix:**
1. Confirm port numbers in **Tools → Zoom Plugin Settings** match your client (defaults: TCP 19870, OSC 19871).
2. Both servers bind to `127.0.0.1` (loopback) — external connections are not supported by default.
3. Check for `TCP control server unavailable` or `OSC server unavailable` in the OBS log — another process may own the port.

---

## Feature Requests

Feature requests are welcome as GitHub issues. Search existing issues first. Label your request `enhancement`.

---

## Zoom Enhanced Media License

CoreVideo requires a **Zoom Enhanced Media License** on the Zoom account used to join meetings.

- Contact your Zoom account representative
- Visit [Zoom Plans](https://zoom.us/pricing)
- Review the [Zoom Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement)

---

## Security

For security vulnerabilities, **do not open a public issue**. See [SECURITY.md](https://github.com/iamfatness/CoreVideo/blob/main/SECURITY.md) for the responsible disclosure process.
