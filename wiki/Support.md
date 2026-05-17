# Support

## Documentation

Before opening an issue, check the full documentation site — most configuration questions and architecture details are covered there:

📖 **[https://corevideo.iamfatness.us/documentation/](https://corevideo.iamfatness.us/documentation/)**

| Topic | Link |
|---|---|
| Installation & build | [Installation](https://corevideo.iamfatness.us/documentation/#installation) |
| Configuration | [Configuration](https://corevideo.iamfatness.us/documentation/#configuration) |
| Zoom Control Dock | [Zoom Control Dock](https://corevideo.iamfatness.us/documentation/#zoom-dock) |
| Assignment modes (Spotlight, Active Speaker, Screen Share) | [Assignment Modes](https://corevideo.iamfatness.us/documentation/#assignment-modes) |
| Active speaker debounce | [Active Speaker Mode](https://corevideo.iamfatness.us/documentation/#active-speaker) |
| Auto-reconnect | [Auto-Reconnect](https://corevideo.iamfatness.us/documentation/#auto-reconnect) |
| Hardware video acceleration | [Hardware Video Acceleration](https://corevideo.iamfatness.us/documentation/#hw-accel) |
| TCP control API | [TCP Control API](https://corevideo.iamfatness.us/documentation/#control-api) |
| OSC control API | [OSC Control API](https://corevideo.iamfatness.us/documentation/#osc-api) |
| IPC protocol reference | [IPC Protocol](https://corevideo.iamfatness.us/documentation/#ipc-protocol) |

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

### Raw-data permission or stream-count error at runtime

**Cause:** Raw data access is available through Zoom Meeting SDK apps, but the signed-in account and app entitlements still determine negotiated quality, bandwidth, and stream count. Standard accounts are typically limited to a 30 Mbps incoming video budget; Enhanced Media / HBM can raise that envelope to roughly 100 Mbps. Developers may also need an app-level entitlement flag to test more than a small number of concurrent raw streams.

**Fix:** Verify the Meeting SDK app is approved or beta-enabled for the account joining the meeting, then confirm the expected bandwidth and developer/app entitlements with Zoom. Treat Enhanced Media / HBM as a production quality and bandwidth tier, not as a hard prerequisite for raw data.

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

**Cause:** The underlying error (auth failure, app entitlement issue, network) is permanent.

**Fix:**
1. Check the OBS log for the `RecoveryReason` and error codes.
2. If the reason is `LicenseError`, see the raw-data permission or stream-count issue above.
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

## Zoom Bandwidth and Enhanced Media / HBM

CoreVideo does not require Enhanced Media / HBM simply to access Meeting SDK raw data. Quality and concurrency are still bounded by Zoom account and app entitlements:

- Standard accounts commonly operate within a 30 Mbps incoming video envelope.
- Enhanced Media / HBM can raise the incoming video envelope to roughly 100 Mbps.
- Standard 1080p feeds are typically about 4-6 Mbps each.
- 100 Mbps can support roughly 16 standard 1080p feeds, or about 8 high-bitrate / 60 fps feeds.
- Developers may need a separate app entitlement flag for testing more than a small number of raw streams; end users should not need that developer flag.

---

## Security

For security vulnerabilities, **do not open a public issue**. See [SECURITY.md](https://github.com/iamfatness/CoreVideo/blob/main/SECURITY.md) for the responsible disclosure process.
