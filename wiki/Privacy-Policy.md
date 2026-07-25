# Privacy Policy

_Last updated: May 2026_

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

### Credentials and Tokens

Published CoreVideo builds use Zoom Public Client OAuth + PKCE through the CoreVideo broker. End users do not enter Zoom app client secrets.

The following local settings may be saved by the plugin:

| Credential / setting | Storage location | Purpose |
|---|---|---|
| Zoom OAuth access/refresh tokens | OBS plugin config directory | Zoom sign-in, token refresh, and ZAK requests |
| Control server token | OBS plugin config directory | Authenticating TCP/OSC API clients |
| Control server ports | OBS plugin config directory | TCP JSON and UDP OSC port configuration |
| Output profiles | OBS plugin config directory | Optional participant-to-source mappings |

On Windows, OAuth tokens are DPAPI-protected before storage. Meeting SDK client secrets are not stored in the plugin or broker for the public-client production path.

---

## Third-Party Services

### Zoom Meeting SDK

CoreVideo uses the **Zoom Meeting SDK** to join and capture meeting content. When joining a meeting, your machine connects to Zoom's infrastructure. Zoom's own privacy policy governs all data exchanged with Zoom's servers:

- [Zoom Privacy Policy](https://explore.zoom.us/en/privacy/)
- [Zoom Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement)

### CoreVideo OAuth Broker

The broker at `corevideo.iamfatness.us` is used only for Zoom OAuth token exchange and refresh. It does not receive or process meeting audio, video, screen share, or participant media.

### Cloudflare and GitHub (Documentation)

The documentation site at `corevideo.io` is served through Cloudflare and sourced from the public CoreVideo GitHub repository. Cloudflare's and GitHub's privacy policies apply to visits to that site:

- [Cloudflare Privacy Policy](https://www.cloudflare.com/privacypolicy/)
- [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)

---

## Data Retention

- **No meeting media is retained by CoreVideo** beyond the local OBS session unless the operator records or streams through OBS.
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
