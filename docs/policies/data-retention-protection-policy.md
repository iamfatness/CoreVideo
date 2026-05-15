# Data Retention and Protection Policy

**Document ID:** CVP-04  
**Version:** 1.0  
**Effective Date:** 2026-05-15  
**Owner:** CoreVideo Security Team  
**Review Cycle:** Annual

---

## 1. Purpose

This policy defines how data processed by CoreVideo is retained, protected, and disposed of, and the obligations of operators regarding data that flows through the plugin.

## 2. Scope

This policy covers all data categories processed by CoreVideo:
- Meeting media (video, audio, screen share)
- Participant roster information
- SDK credentials and configuration
- Output profiles
- Log files and diagnostic data
- Shared memory regions

## 3. Data Classification

| Classification | Examples | Handling Requirement |
|---|---|---|
| **Restricted** | SDK Key, SDK Secret, control server token, meeting passcode | Stored only in OS-protected config; never logged; never transmitted except to Zoom auth endpoint |
| **Confidential** | Participant display names, user IDs, roster state | In-memory only during session; cleared on leave; treated as PII under applicable law |
| **Internal** | Output profile JSON files | Stored on local filesystem; operator controls access; may contain historical participant data |
| **Public** | Plugin version, build metadata, open-source code | No restrictions |

## 4. Retention Schedule

### 4.1 In-Memory Data (Session Data)
| Data Type | Retention Period | Disposal |
|---|---|---|
| Video / audio frames | Single frame lifetime (≤ 33 ms at 30 fps) | Overwritten by next frame in shared memory ring |
| Participant roster | Duration of meeting session | Cleared in-memory on `left` IPC event or OBS close |
| Active speaker state | Duration of meeting session | Cleared on meeting leave |
| Meeting credentials (ID, passcode) | Duration of join attempt | Cleared from ZoomReconnectManager storage after successful join or final failure |
| JWT tokens | Single use (≤ 2-hour validity) | Discarded after SDK auth call |

### 4.2 Persistent Data (Filesystem)
| Data Type | Storage Location | Default Retention | Disposal Procedure |
|---|---|---|---|
| SDK Key & Secret | `obs-studio/plugin_config/obs-zoom-plugin/settings.json` | Until manually deleted by operator | Operator deletes file or clears field in Settings dialog |
| Control server token | Same settings file | Until manually deleted | Same as above |
| Output profiles | `obs-studio/plugin_config/obs-zoom-plugin/profiles/*.json` | Until manually deleted via Output Manager | Delete via Output Manager dialog or remove files directly |
| OBS recording / stream | Operator-configured OBS output path | Operator-defined | Operator's responsibility |

### 4.3 Shared Memory
Named shared memory regions (prefixed `ZoomObsPlugin_<UUID>`) are created at session start and destroyed when the ZoomObsEngine process exits. They do not persist across reboots. On Linux, `/dev/shm/` entries are removed automatically on process exit.

### 4.4 No Server-Side Retention
CoreVideo does not transmit data to any server operated by the CoreVideo project. No cloud retention schedule applies.

## 5. Data Protection Controls

### 5.1 Credentials
- SDK Key, Secret, and control server token are stored in the OBS plugin configuration file with default filesystem permissions (owner read/write only on POSIX; user-profile restricted on Windows).
- Credentials are never written to log files, debug output, or IPC messages.
- JWT tokens are generated in-process and used once; the raw secret is never transmitted.

### 5.2 Participant Data
- Participant display names are handled as opaque strings; they are never interpolated into shell commands, SQL queries, or format strings.
- Participant user IDs are treated as opaque `uint32_t` values; no personal profile information is derived from them.
- Roster data is held in heap memory and cleared when the `left` event is received from the engine.

### 5.3 Shared Memory
- Shared memory region names include a random UUID generated per session, preventing predictable naming and reducing risk of local privilege escalation via SHM name collision.
- On POSIX, shared memory is created with `O_CREAT | O_EXCL` and mode `0600` (owner-only access).
- On Windows, named shared memory objects are created with a security descriptor limiting access to the creating user's session.

### 5.4 Output Profiles
- Profile JSON files may contain participant display names and user IDs captured at the time the profile was saved.
- Operators should apply filesystem access controls appropriate to the sensitivity of participant information.
- Profiles can be deleted via **OBS → Tools → Zoom Output Manager** or by removing files from the profiles directory.

### 5.5 Encryption
- Meeting media is transmitted by the Zoom SDK over Zoom's encrypted transport (AES-256); CoreVideo receives already-decrypted raw frames via the SDK raw data API.
- No additional encryption is applied to shared memory regions, as they are local to the machine and access-controlled by the OS.
- Settings files are not encrypted at rest. Operators requiring at-rest encryption should use OS-level disk encryption (BitLocker, FileVault, dm-crypt).

## 6. Data Subject Rights

CoreVideo is software, not a data controller. Operators deploying CoreVideo are the data controllers under GDPR, CCPA, and similar regulations. Meeting participants who wish to exercise data subject rights (access, erasure, portability) must contact the operator of the meeting where CoreVideo was in use.

## 7. Breach Notification

If an operator discovers that credentials or participant data have been exposed due to a vulnerability in CoreVideo, they should:
1. Immediately revoke and regenerate the exposed credentials (Zoom SDK Key/Secret via the Zoom Developer Portal).
2. Report the vulnerability to the CoreVideo project via the private security reporting channel.
3. Notify affected participants in accordance with applicable law and the operator's own breach notification obligations.

## 8. Operator Responsibilities

Operators are solely responsible for:
- Configuring OS-level file permissions on the OBS plugin configuration directory.
- Applying at-rest encryption if required by their regulatory environment.
- Complying with applicable data protection laws (GDPR, CCPA, PIPEDA, etc.) with respect to meeting participants.
- Managing the retention and deletion of OBS recordings created with CoreVideo output.
- Obtaining valid consents and notices required by law before capturing participant video and audio.
