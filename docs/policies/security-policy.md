# Security Policy

**Document ID:** CVP-02  
**Version:** 1.0  
**Effective Date:** 2026-05-15  
**Owner:** CoreVideo Security Team  
**Review Cycle:** Annual

---

## 1. Purpose

This policy defines the security principles, controls, and obligations that govern the design, development, release, and operation of CoreVideo.

## 2. Scope

This policy applies to:
- All CoreVideo source code, build artefacts, and released binaries
- The CoreVideo GitHub repository and associated CI/CD pipelines
- All contributors, maintainers, and operators of CoreVideo

## 3. Security Principles

CoreVideo is designed around the following security principles:

| Principle | Implementation |
|---|---|
| **Least Privilege** | The OBS plugin process holds no Zoom SDK access; all SDK interaction is isolated to the ZoomObsEngine child process |
| **Process Isolation** | ZoomObsEngine runs as a separate OS process; a crash cannot corrupt OBS memory |
| **Defence in Depth** | IPC input is validated at both ends; participant IDs are treated as opaque integers; display names are never interpolated into commands |
| **Secure Defaults** | Control server token authentication is enabled by default; OSC and TCP servers bind to loopback only |
| **Minimal Attack Surface** | No inbound network listeners accept external connections; all APIs bind to 127.0.0.1 |
| **Credential Hygiene** | SDK credentials are stored in OBS plugin config (not source); JWTs are generated locally and not logged |
| **Fail Safe** | On engine crash or IPC failure, ZoomReconnectManager attempts recovery; sources show placeholder frames rather than stale data |

## 4. Supported Versions

Security fixes are applied to the `main` branch. Once a stable release channel exists, the two most recent minor versions will receive security backports.

| Branch / Version | Supported |
|---|---|
| `main` | ✓ Active |
| Tagged releases (≥ 1.0) | ✓ Current + previous minor |
| Older releases | ✗ No support |

## 5. Reporting a Vulnerability

**Do not open public GitHub issues for security vulnerabilities.**

Report security concerns privately to:
- **GitHub Private Security Advisory:** [github.com/iamfatness/CoreVideo/security/advisories/new](https://github.com/iamfatness/CoreVideo/security/advisories/new)

Include:
1. Affected component and version
2. Description of the vulnerability
3. Steps to reproduce or proof-of-concept
4. Assessed impact (confidentiality, integrity, availability)
5. Any suggested mitigations

The maintainer will acknowledge valid reports within **5 business days** and provide a remediation timeline.

## 6. Security Controls

### 6.1 Code Security
- All IPC messages are parsed as JSON with explicit field validation; malformed messages are discarded
- Participant display names are never used in shell commands, SQL, or format strings
- Shared memory regions use UUID-based names to prevent prediction
- Constant-time token comparison in the TCP control server prevents timing attacks
- SIGPIPE is silenced on POSIX to prevent crash-on-broken-pipe

### 6.2 Credential Security
- SDK Key and Secret are stored in the OBS plugin configuration directory with OS-level access controls
- JWT tokens are generated in-process from Key+Secret using HMAC-SHA256; no plaintext secret is transmitted
- The `zoom-credentials.h.in` template contains only placeholders; no real credentials are committed to VCS
- GitHub Secret Scanning is active on the repository

### 6.3 Build Security
- Releases are built from tagged commits on the `main` branch
- Build artefacts are produced by the CI pipeline; no local-only builds are published as official releases
- Compiler hardening flags are applied: `-D_FORTIFY_SOURCE=2`, stack protector, position-independent code

### 6.4 Dependency Security
- Third-party dependencies are pinned to specific versions in CMakeLists.txt
- The Zoom SDK is obtained directly from the official Zoom Developer Portal
- Dependabot alerts are monitored for known CVEs in transitive dependencies

## 7. Compliance

CoreVideo is developed to meet the security requirements of the [Zoom App Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement) and the [Zoom Meeting SDK Terms of Use](https://explore.zoom.us/en/isv-license-agreement/).

## 8. Policy Violations

Contributors who knowingly commit credentials, bypass security controls, or suppress security tooling without documented justification may have their access revoked and their contributions reverted.
