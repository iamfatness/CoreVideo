# Privacy Policy

_Last updated: May 2026_

## Overview

CoreVideo is an open-source OBS Studio plugin. This privacy policy explains what data the plugin processes, where it goes, and what is stored.

---

## Data Processed

### Meeting Audio and Video

CoreVideo receives raw video (I420 YUV) and audio (48 kHz PCM) streams from the Zoom Meeting SDK. These streams are:

- Processed **locally on the operator's machine only**
- Delivered directly to OBS Studio as native source frames
- **Never transmitted to any CoreVideo server, third-party service, or remote endpoint**

### Participant Roster

CoreVideo receives participant metadata from the Zoom SDK (display names, user IDs, mute/video/talking state, host/co-host status, spotlight position). This information is:

- Held in memory only for the duration of the meeting session
- Displayed within OBS for source assignment purposes
- **Not stored to disk, logged, or transmitted outside the local machine**

### Credentials

The following credentials may be saved locally by the plugin:

| Credential | Storage location | Purpose |
|---|---|---|
| Zoom SDK App Key | OBS plugin config directory | Generating Meeting SDK JWTs |
| Zoom SDK App Secret | OBS plugin config directory | Generating Meeting SDK JWTs |
| JWT override token | OBS plugin config directory | Optional manual JWT override |
| Control server token | OBS plugin config directory | Authenticating TCP/OSC API clients |
| Control server ports | OBS plugin config directory | TCP JSON and UDP OSC port configuration |

Credentials are stored in the OBS plugin configuration directory on the operator's local machine and are **not transmitted to any remote service by CoreVideo**.

---

## Third-Party Services

### Zoom Meeting SDK

CoreVideo uses the **Zoom Meeting SDK** to join and capture meeting content. When joining a meeting, your machine connects to Zoom's infrastructure (HTTPS/WSS). Zoom's own privacy policy governs all data exchanged with Zoom's servers:

- [Zoom Privacy Policy](https://explore.zoom.us/en/privacy/)
- [Zoom Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement)

### Cloudflare and GitHub (Documentation)

The documentation site at `corevideo.iamfatness.us` is served through Cloudflare and sourced from the public CoreVideo GitHub repository. Cloudflare's and GitHub's privacy policies apply to visits to that site:

- [Cloudflare Privacy Policy](https://www.cloudflare.com/privacypolicy/)
- [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)

---

## Data Retention

- **No user data is retained by CoreVideo** beyond the local OBS session.
- Credentials saved in the OBS config directory persist until deleted via the Settings dialog or plugin removal.
- OBS itself may retain scenes, sources, and recordings per its own configuration — outside the scope of this policy.

---

## Children's Privacy

CoreVideo is a professional broadcast tool not directed at children. No data from minors is knowingly collected.

---

## Changes to This Policy

Updates will be reflected in the `Last updated` date above. Significant changes will be noted in the [release notes](https://github.com/iamfatness/CoreVideo/releases).

---

## Contact

For privacy questions, open an issue at [github.com/iamfatness/CoreVideo/issues](https://github.com/iamfatness/CoreVideo/issues) with the label `privacy`.
