# CoreVideo Wiki

CoreVideo is an OBS Studio plugin that pulls Zoom meeting video, audio, screen
share, and Zoom interpretation audio into OBS as native sources - no NDI, no
virtual camera, no screen capture of a Zoom window. Latest release: **v0.1.44**
(2026-08-22).

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

**Windows 10/11 x64 is the only supported, packaged platform.** It is the only
configuration with a release pipeline, and the only one the maintainers build,
test, and run in production.

The CMake project configures on macOS, Linux, and Windows arm64, and CI compiles
the cross-platform sources on macOS and Linux to catch portability regressions -
but none of those produce an installable build. The one macOS bundle ever
published was `v0.1.32-beta.1`, which is many releases behind and should not be
used for a show. See
[Platform Support](https://github.com/iamfatness/CoreVideo/blob/main/README.md#platform-support)
for what a source build on those platforms does and does not get you.

CoreVideo is in public beta and the installer is not code-signed yet, so Windows
SmartScreen flags it on first run.

---

## Getting help

Start with [Support](Support). It covers where the logs are, how to produce a
redacted support bundle from the **Zoom Diagnostics** dock, and the failures
that come up most often. A bug report with a support bundle attached is worth
several without one.

CoreVideo is an independent MIT-licensed project and is not affiliated with,
endorsed by, or supported by Zoom Video Communications, Inc.
