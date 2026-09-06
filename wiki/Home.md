# CoreVideo Wiki

CoreVideo is an OBS Studio plugin that pulls Zoom meeting video, audio, screen
share, and Zoom interpretation audio into OBS as native sources - no NDI, no
virtual camera, no screen capture of a Zoom window. Recommended Windows release: **v0.1.44**; macOS Apple Silicon beta: **v0.1.45-beta.1**
(replacement installer published September 6, 2026).

This wiki holds the policy and support pages. Everything operational - install,
configuration, architecture, the control APIs - lives on the documentation site,
which is generated from the same repository and is always the more current of
the two.

| | |
|---|---|
| **Full documentation** | https://corevideo.io/documentation/ |
| **Plugin guide** (sources, assignment, talkback, ISO recording, control APIs) | https://corevideo.io/core-plugin/ |
| **Downloads** | https://corevideo.io/download/ |

---

## Pages here

- [Support](Support) - troubleshooting, log collection, how to file a useful bug
- [Privacy Policy](Privacy-Policy)
- [Terms of Use](Terms-of-Use)

---

## Where things are

| Resource | Link |
|---|---|
| Latest release (Windows installer + ZIP) | https://github.com/iamfatness/CoreVideo/releases/latest |
| Changelog | https://github.com/iamfatness/CoreVideo/blob/main/CHANGELOG.md |
| Source code | https://github.com/iamfatness/CoreVideo |
| Issues and bug reports | https://github.com/iamfatness/CoreVideo/issues |
| Security disclosure | https://github.com/iamfatness/CoreVideo/blob/main/SECURITY.md |
| Roadmap | https://github.com/iamfatness/CoreVideo/blob/main/docs/ROADMAP.md |
| Zoom OAuth / Marketplace setup | https://corevideo.io/documentation/#flow-oauth |
| Bitfocus Companion module | https://github.com/iamfatness/CoreVideo/tree/main/companion |

---

## Platform status

**Windows 10/11 x64 and macOS Apple Silicon have packaged builds.** Windows
uses the stable release channel; macOS v0.1.45-beta.1 is a public beta with a
Developer ID signed, notarized installer. [Download and install](https://corevideo.io/download/#macos).
The original macOS package was replaced on September 6, 2026 after signature
errors; download the replacement if you already have an earlier copy. Intel
Macs are not supported by this package, and Linux requires a source build.

CoreVideo is in public beta. The Windows installer is not code-signed yet,
so Windows SmartScreen flags it on first run.

---

## Getting help

Start with [Support](Support). It covers where the logs are, how to produce a
redacted support bundle from the **Zoom Diagnostics** dock, and the failures
that come up most often. A bug report with a support bundle attached is worth
several without one.

CoreVideo is an independent MIT-licensed project and is not affiliated with,
endorsed by, or supported by Zoom Video Communications, Inc.
