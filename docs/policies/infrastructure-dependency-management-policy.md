# Infrastructure and Dependency Management Policy

**Document ID:** CVP-06  
**Version:** 1.0  
**Effective Date:** 2026-05-15  
**Owner:** CoreVideo Security Team  
**Review Cycle:** Annual

---

## 1. Purpose

This policy defines how CoreVideo manages its build infrastructure, CI/CD pipelines, and third-party software dependencies to minimise supply-chain risk and maintain a secure, reproducible build process.

## 2. Scope

- Build infrastructure: GitHub Actions CI/CD, CMake build system
- Third-party libraries: Zoom Meeting SDK, OBS SDK, Qt, FFmpeg (optional), nlohmann/json, tinyosc
- Developer toolchain: compilers, static analysers, packaging tools
- GitHub repository settings: branch protection, access controls, secret scanning

## 3. Dependency Inventory

### 3.1 Runtime Dependencies

| Dependency | Version Constraint | Source | License | Security Notes |
|---|---|---|---|---|
| Zoom Meeting SDK | 5.17.x or 7.x | Zoom Developer Portal (official) | Zoom ISV Agreement | Obtain only from official portal; verify download integrity |
| OBS Studio (libobs) | ≥ 30 | OBS Project official releases | GPL-2.0 | Build-time dependency; no runtime linkage for ZoomObsEngine |
| Qt | 6.x | Qt official installer or distro package | LGPL-3.0 | UI only (plugin process); not linked into engine |
| FFmpeg | ≥ 5.0 (optional) | Distro package or official build | LGPL-2.1 | Only when `-DCOREVIDEO_HW_ACCEL=ON`; hw video pipeline only |

### 3.2 Header-Only / Bundled Dependencies

| Dependency | Version | Location | Notes |
|---|---|---|---|
| nlohmann/json | Pinned in CMakeLists | Fetched via CMake FetchContent or vendored | JSON IPC parsing; no native code |
| tinyosc | Pinned commit | Vendored in `third_party/` | OSC packet parsing; small, audited |

### 3.3 Build / Toolchain Dependencies

| Tool | Minimum Version | Purpose |
|---|---|---|
| CMake | 3.16 | Build system |
| MSVC | 2022 (v143) | Windows compilation |
| Clang | 14+ | macOS / Linux compilation |
| GCC | 11+ | Linux compilation |
| Cppcheck | 2.13+ | SAST — run in CI |
| Flawfinder | 2.0.19+ | SAST — run before release |

## 4. Dependency Acquisition and Verification

### 4.1 Approved Sources
Dependencies must be obtained from the following approved sources only:

| Dependency | Approved Source |
|---|---|
| Zoom Meeting SDK | [developers.zoom.us](https://developers.zoom.us/docs/meeting-sdk/releases/) — authenticated download |
| OBS SDK | Official OBS GitHub releases or distro packages |
| Qt | [qt.io](https://www.qt.io) official installer or verified distro package |
| FFmpeg | Official FFmpeg builds or distro packages (e.g., `apt`, `brew`, `vcpkg`) |
| nlohmann/json | GitHub release tarball via CMake FetchContent with SHA-256 verification |
| tinyosc | Vendored with commit hash pinned in CMakeLists |

### 4.2 Integrity Verification
- For CMake FetchContent dependencies: SHA-256 content hash is specified in CMakeLists.txt and verified at fetch time.
- For vendored dependencies: the commit SHA is recorded in CMakeLists.txt; changes require maintainer review.
- For SDK downloads: checksums published by Zoom on the developer portal must be verified before use in CI.

### 4.3 Prohibited Sources
- Unofficial mirrors, forks, or third-party redistributions of the Zoom SDK are not permitted.
- Dependencies must not be fetched from unauthenticated HTTP endpoints.
- Pre-built binaries from unofficial sources must not be incorporated without explicit maintainer approval and documented security review.

## 5. Dependency Update Policy

| Dependency Class | Update Trigger | Approval Required |
|---|---|---|
| Security patch (CVE fix) | Immediately on CVE publication | Maintainer review; expedited |
| Zoom SDK minor update | On official Zoom release | Maintainer review; regression test required |
| Zoom SDK major update | When required for marketplace compliance | Full integration test required |
| OBS SDK update | When OBS bumps minimum supported version | Compatibility test required |
| Qt update | Quarterly or on security advisory | Build and UI regression test |
| Toolchain (compiler) | Annually or on security advisory | CI pipeline update |

### 5.1 Vulnerability Remediation
When Dependabot or NVD identifies a CVE in a dependency:
1. Assess exploitability in the CoreVideo context (local tool vs. runtime library).
2. Identify the patched version.
3. Update the version pin in CMakeLists.txt.
4. Run the full SAST suite and test matrix.
5. Issue a patch release if the CVE is rated Medium or higher.

## 6. Build Infrastructure

### 6.1 GitHub Repository Security
- **Branch protection:** `main` branch requires pull-request review before merge; direct pushes are disabled for non-maintainers.
- **Signed commits:** Maintainer commits to `main` are GPG-signed.
- **Secret Scanning:** GitHub Secret Scanning push protection is enabled; pushes containing credential patterns are blocked.
- **Dependabot:** Enabled for GitHub Actions workflow dependencies.

### 6.2 CI/CD Pipeline
- All CI workflows run in GitHub Actions using pinned action versions (SHA pinning preferred over tag pinning).
- CI secrets (if any) are stored as GitHub Actions encrypted secrets; they are never echoed in logs.
- SAST (Cppcheck) runs on every pull request; failures block merge.
- Release builds are triggered only from signed tags on `main`.

### 6.3 Build Reproducibility
- CMakeLists.txt specifies exact dependency versions; no floating `latest` pins in production builds.
- Build configurations are documented in `buildspec/` for each supported platform.
- Release binaries are produced by CI; no developer machines are used for official release builds.

## 7. Software Bill of Materials (SBOM)

An SBOM is generated at each release using CMake dependency tracking and published alongside the release artefacts. The SBOM lists:
- All direct and transitive dependencies
- Version and commit SHA for each
- SPDX license identifiers
- Known CVEs at time of release (from NVD lookup)

## 8. End-of-Life and Deprecation

| Scenario | Action |
|---|---|
| Dependency reaches EOL | Migrate to supported alternative within 90 days |
| Zoom SDK version deprecated by Zoom | Migrate within 60 days or before Zoom enforcement date |
| Compiler version EOL | Update CI toolchain; drop support for EOL compilers |
| OS platform EOL | Announce deprecation in CHANGELOG; remove from CI matrix |

## 9. Roles and Responsibilities

| Role | Responsibility |
|---|---|
| Maintainer | Approve dependency updates; own CMakeLists.txt; manage CI secrets |
| Contributors | Propose dependency updates via PR; include security rationale; run SAST locally |
| CI/CD Pipeline | Enforce version pins; run SAST; block merge on failure |
