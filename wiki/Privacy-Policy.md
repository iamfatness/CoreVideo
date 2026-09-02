# Privacy Policy

_Last updated: September 2026_

## Overview

CoreVideo is an open-source OBS Studio plugin. This privacy policy explains what data the plugin processes, where it goes, and what is stored.

---

## Data Processed

### Meeting Audio and Video

CoreVideo receives raw video (I420 YUV), screen share, interpretation audio, and audio (48 kHz PCM) streams from the Zoom Meeting SDK. These streams are:

- Processed **locally on the operator's machine only**
- Delivered directly to OBS Studio as native source frames
- **Never transmitted to any CoreVideo server, third-party service, or remote endpoint**

### Participant Roster

CoreVideo receives participant metadata from the Zoom SDK (display names, user IDs, mute/video/talking state, host/co-host status, spotlight position). This information is:

- Held in memory only for the duration of the meeting session
- Displayed within OBS for source assignment purposes
- Not transmitted outside the local machine by CoreVideo

### Talkback (Intercom) Audio

CoreVideo's talkback feature is the one path where audio travels **outward**. When
the operator holds or latches a talkback key, CoreVideo reads the OBS audio source
the operator selected and sends it to Zoom over the Zoom Meeting SDK's own
talkback channels, so that the selected participants hear it. This audio goes to
Zoom and to no one else; it is not sent to any CoreVideo server or third party,
and CoreVideo does not record it.

Two properties are worth stating plainly because they are not obvious:

- Talkback requires CoreVideo's own meeting microphone to be unmuted, so
  CoreVideo unmutes itself for the duration of a key and restores the previous
  state on release if it was the one that opened it.
- The talkback diagnostic probe sends an audible three-second test tone to the
  selected participant and briefly lowers their meeting audio. Only run it on
  someone who is expecting it.

### Recordings You Make

CoreVideo's ISO recorder writes MP4 video and WAV audio files, plus an FFmpeg log
per session, to a folder the operator chooses. Diagnostic support bundles are
written to the OBS plugin configuration directory. All of these stay on the
operator's machine; CoreVideo never uploads them. A support bundle redacts
credentials but does contain meeting and participant display names, participant
IDs, and recording file paths - review one before sharing it.

The Zoom Meeting SDK is initialised with its own logging and crash dumps enabled,
which it writes locally under its own control. CoreVideo does not set or override
that location and does not collect those files.

### Credentials and Tokens

Published CoreVideo builds use Zoom Public Client OAuth + PKCE through the CoreVideo broker. End users do not enter Zoom app client secrets.

The following local settings may be saved by the plugin:

| Credential / setting | Storage location | Purpose |
|---|---|---|
| Zoom OAuth access/refresh tokens | OBS global configuration, `[ZoomPlugin]` section | Zoom sign-in, token refresh, and ZAK requests |
| Control server token | OBS global configuration, `[ZoomPlugin]` section | Authenticating TCP API clients |
| Control server ports | OBS global configuration, `[ZoomPlugin]` section | TCP JSON and UDP OSC port configuration |
| Talkback preferences | OBS global configuration, `[ZoomPlugin]` section | The chosen talkback audio source name and latch setting |
| Output profiles | OBS plugin config directory | Optional participant-to-source mappings |
| Support bundles | OBS plugin config directory | Local troubleshooting exports, written only when the operator asks |

On Windows, OAuth tokens are DPAPI-protected before storage. Meeting SDK client secrets are not stored in the plugin or broker for the public-client production path.

---

## Third-Party Services

### Zoom Meeting SDK

CoreVideo uses the **Zoom Meeting SDK** to join and capture meeting content. When joining a meeting, your machine connects to Zoom's infrastructure. Zoom's own privacy policy governs all data exchanged with Zoom's servers:

- [Zoom Privacy Policy](https://explore.zoom.us/en/privacy/)
- [Zoom Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement)

### CoreVideo OAuth Broker

The broker at `corevideo.iamfatness.us` is used only for Zoom OAuth token exchange and refresh. It does not receive or process meeting audio, video, screen share, or participant media.

### GitHub (Update Check)

Once per OBS session, CoreVideo makes a single anonymous HTTPS GET request to the
public GitHub Releases API
(`api.github.com/repos/iamfatness/CoreVideo/releases/latest`) to see whether a
newer release is available. The request carries no meeting data, credentials,
telemetry, or CoreVideo-specific identifiers - only what GitHub already logs for
any anonymous HTTP request, such as an IP address, governed by GitHub's own
privacy statement. The check never blocks startup, never downloads or installs
anything, and fails silently when offline. It can be turned off in **Tools ->
Zoom Plugin Settings -> Check for updates on startup**, which is enabled by
default.

### Cloudflare and GitHub (Documentation)

The documentation site at `corevideo.io` is served through Cloudflare and sourced from the public CoreVideo GitHub repository. Cloudflare's and GitHub's privacy policies apply to visits to that site:

- [Cloudflare Privacy Policy](https://www.cloudflare.com/privacypolicy/)
- [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)

---

## Data Retention

- **No meeting media is retained by CoreVideo** beyond the local OBS session unless the operator records or streams through OBS, or uses CoreVideo's own ISO recorder.
- ISO recordings, their FFmpeg logs, and support bundles persist on disk until the operator deletes them. CoreVideo never removes them and never uploads them.
- OAuth tokens persist until the user signs out, revokes access, or removes the plugin configuration.
- Credentials saved in the OBS config directory persist until deleted via the Settings dialog or plugin removal.
- OBS itself may retain scenes, sources, and recordings per its own configuration - outside the scope of this policy.

---

## Children's Privacy

CoreVideo is a professional broadcast tool not directed at children. No data from minors is knowingly collected.

---

## Changes to This Policy

Updates will be reflected in the `Last updated` date above. Significant changes will be noted in the [release notes](https://github.com/iamfatness/CoreVideo/releases).

---

## Contact

For privacy questions, open an issue at [github.com/iamfatness/CoreVideo/issues](https://github.com/iamfatness/CoreVideo/issues) with the label `privacy`.
