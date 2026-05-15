# Privacy Policy

**Document ID:** CVP-03  
**Version:** 1.0  
**Effective Date:** 2026-05-15  
**Owner:** CoreVideo Security Team  
**Review Cycle:** Annual

---

## 1. Overview

CoreVideo is an OBS Studio plugin that integrates the Zoom Meeting SDK to capture raw video and audio from Zoom meetings. This policy explains what data CoreVideo processes, how it is handled, and the obligations of operators who deploy it.

CoreVideo is **software**, not a cloud service. It runs entirely on the operator's local machine. It does not operate servers, collect analytics, or transmit data to Anthropic, the CoreVideo project, or any party other than Zoom.

## 2. Data Processed

### 2.1 Meeting Media
| Data Type | Description | Processing Location |
|---|---|---|
| Raw video frames | I420 YUV frames from participant cameras | Local machine memory / shared memory |
| Raw audio PCM | 48 kHz PCM audio from participant microphones | Local machine memory / shared memory |
| Screen share frames | I420 YUV frames from active screen share | Local machine memory / shared memory |
| Interpretation audio | PCM audio from language interpretation channels | Local machine memory / shared memory |

All media is delivered from the Zoom SDK to OBS in real time. CoreVideo does not write meeting media to disk except as part of OBS recording or streaming, which is configured and controlled exclusively by the operator.

### 2.2 Participant Roster
| Data Type | Description | Retention |
|---|---|---|
| Participant display names | As set by participants in Zoom | In-memory only; cleared on meeting leave |
| Participant user IDs | Opaque uint32_t identifiers assigned by Zoom | In-memory only; cleared on meeting leave |
| Participant state | Video on/off, muted, talking, host/co-host, raised hand, spotlight slot | In-memory only; not persisted |

### 2.3 Credentials
| Data Type | Storage Location | Notes |
|---|---|---|
| Zoom SDK Key | OBS plugin config directory | OS-protected; not transmitted except to Zoom auth endpoint |
| Zoom SDK Secret | OBS plugin config directory | Used locally for JWT generation only; never transmitted |
| Control server token | OBS plugin config directory | Loopback API authentication |
| Meeting passcode | In-memory during join; not persisted | Provided by operator at join time |

### 2.4 Output Profiles
Named output profiles (source-to-participant mappings) are saved as JSON files under `obs-studio/plugin_config/obs-zoom-plugin/profiles/`. These files contain participant display names and user IDs as of the time the profile was saved. Operators should treat these files with appropriate access controls.

## 3. Data CoreVideo Does NOT Collect

- CoreVideo does not collect analytics, telemetry, or usage statistics.
- CoreVideo does not transmit meeting media, participant data, or credentials to any party other than Zoom (via the Zoom Meeting SDK).
- CoreVideo does not use cookies, tracking pixels, or persistent identifiers.
- CoreVideo does not create accounts or user profiles.

## 4. Third-Party Services

### 4.1 Zoom Meeting SDK
All meeting media and participant data flows through the Zoom Meeting SDK, which is subject to [Zoom's Privacy Statement](https://explore.zoom.us/en/privacy/). Operators must obtain appropriate consent from meeting participants before recording or streaming their video and audio.

### 4.2 GitHub
The CoreVideo source code and documentation are hosted on GitHub. GitHub's [Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement) governs data associated with repository access and GitHub Pages.

### 4.3 No Other Third-Party Services
CoreVideo does not integrate with analytics platforms, advertising networks, or cloud storage services.

## 5. Operator Obligations

Operators deploying CoreVideo in a production environment are responsible for:

1. **Participant consent** — Informing meeting participants that their video and audio are being captured and streamed or recorded. This is a legal requirement in most jurisdictions and a condition of the Zoom Meeting SDK Terms of Use.
2. **Credential protection** — Securing the OBS plugin configuration directory to prevent unauthorised access to SDK credentials.
3. **Recording compliance** — Ensuring that any OBS recording or stream created using CoreVideo complies with applicable laws, Zoom's Terms of Service, and the operator's organisational policies.
4. **Enhanced Media License** — Confirming that the Zoom account used holds a valid Zoom Enhanced Media License (or equivalent) before deploying CoreVideo in production.

## 6. Children's Privacy

CoreVideo is designed for professional broadcast and production use. It is not directed at children under 13 (or the applicable age of digital consent in the operator's jurisdiction). Operators are solely responsible for ensuring that any meeting participants are of appropriate age.

## 7. Data Retention

All data processed by CoreVideo is held in-process memory and is cleared when the meeting ends or OBS is closed. No meeting media, roster data, or credentials are written to persistent storage by CoreVideo itself. See the Data Retention and Protection Policy (CVP-04) for detail on output profiles and log files.

## 8. Security

CoreVideo implements the security controls described in the Security Policy (CVP-02), including process isolation, loopback-only APIs, constant-time token comparison, and UUID-based shared memory naming.

## 9. Changes to This Policy

Material changes to this policy will be announced via the CoreVideo GitHub repository. Continued use of CoreVideo after a policy update constitutes acceptance of the revised terms.

## 10. Contact

Privacy questions or concerns regarding CoreVideo may be raised via a GitHub issue on the CoreVideo repository or via the private security reporting channel described in the Security Policy.
