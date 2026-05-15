# Incident Management and Response Policy

**Document ID:** CVP-05  
**Version:** 1.0  
**Effective Date:** 2026-05-15  
**Owner:** CoreVideo Security Team  
**Review Cycle:** Annual

---

## 1. Purpose

This policy defines the process for identifying, containing, remediating, and learning from security incidents affecting CoreVideo software, its users, and its supply chain.

## 2. Scope

This policy covers security incidents involving:
- Vulnerabilities in CoreVideo source code or released binaries
- Compromise of the CoreVideo GitHub repository or CI/CD pipeline
- Accidental exposure of credentials or secrets in the repository
- Third-party dependency vulnerabilities with material impact on CoreVideo
- Reports of active exploitation of CoreVideo in the wild

## 3. Incident Classification

| Severity | Criteria | Examples |
|---|---|---|
| **P1 — Critical** | Active exploitation; credential breach; supply chain compromise | Compromised release binary, exposed SDK secret in public repo, active RCE exploit |
| **P2 — High** | Confirmed vulnerability with high CVSS; significant user impact | Privilege escalation via IPC, authentication bypass on control server |
| **P3 — Medium** | Confirmed vulnerability with limited scope or requiring local access | Information disclosure via SHM naming, DoS of local control API |
| **P4 — Low** | Best-practice deviation; no confirmed exploitability | Missing compiler hardening flag, informational SAST finding |

## 4. Incident Response Phases

### Phase 1 — Detection and Triage (Target: ≤ 24 hours for P1/P2)

**Inputs:**
- Responsible disclosure report via GitHub Security Advisory
- GitHub Secret Scanning or Dependabot alert
- SAST tool output from CI pipeline
- User bug report mentioning security behaviour

**Actions:**
1. Assign incident owner (maintainer or designated responder).
2. Classify severity using the table in Section 3.
3. Open a private GitHub Security Advisory for tracking.
4. Acknowledge receipt to reporter (if external) within 5 business days.

### Phase 2 — Containment (Target: ≤ 24 hours for P1, ≤ 7 days for P2)

| Scenario | Containment Action |
|---|---|
| Exposed credential in repo | Immediately revoke credential at provider; force-push to remove from history (or use git filter-repo); enable GitHub Secret Scanning push protection |
| Vulnerable release binary | Remove or mark release as deprecated on GitHub Releases; add advisory banner to README |
| Compromised repository access | Revoke affected personal access tokens; audit recent commits and workflow runs |
| Active exploit in the wild | Publish emergency advisory; coordinate with Zoom security team if SDK involvement |
| Dependency CVE | Pin to patched version in CMakeLists.txt; issue patch release |

### Phase 3 — Investigation and Root Cause Analysis

1. Reproduce the issue in an isolated environment.
2. Identify the root cause (code defect, misconfiguration, supply chain, social engineering).
3. Determine scope: which versions are affected, what data or capabilities are at risk.
4. Document findings in the private advisory.

### Phase 4 — Remediation and Patch Development

1. Develop fix on a private branch (or public branch for non-sensitive issues).
2. Run full SAST suite (Cppcheck + Flawfinder) against the fix.
3. Conduct security-focused code review by at least one additional reviewer.
4. Build and test on all supported platforms (Windows, macOS, Linux) before release.
5. Prepare release notes and security advisory text.

### Phase 5 — Recovery and Disclosure

1. Publish patched release with a security advisory in the GitHub Security Advisories tab.
2. Credit reporter (unless anonymity requested).
3. Request CVE from MITRE where CVSS ≥ 4.0 or where a CVE is needed for downstream tracking.
4. Notify affected operators via a pinned GitHub notice or release announcement.
5. Update the SECURITY.md supported-versions table if versions are being deprecated.

### Phase 6 — Post-Incident Review (Within 30 days of closure)

1. Document timeline, root cause, and lessons learned in an internal retrospective.
2. Update SAST rules, code review checklist, or CI gates to prevent recurrence.
3. Record the incident in the audit trail (see Audit Trail section of SSDP).

## 5. Communication Standards

| Audience | Channel | Timing |
|---|---|---|
| Reporter | GitHub Security Advisory DM | Acknowledge within 5 business days; update on major milestones |
| Zoom security team | Zoom Developer Support portal | Immediately for incidents involving SDK behaviour |
| Public / operators | GitHub Security Advisory (published) | Upon patch release or after 90-day coordinated disclosure window |
| GitHub platform | GitHub Security Advisory metadata | At publication for CVE request |

**Embargo:** Security information is kept confidential until a patch is available or 90 days have elapsed from the initial report, whichever comes first. The 90-day limit may be extended by mutual agreement with the reporter.

## 6. Roles and Responsibilities

| Role | Responsibilities |
|---|---|
| **Incident Owner** (Maintainer) | Triage, own timeline, coordinate patch, publish advisory |
| **Responder** (Contributor) | Technical investigation, patch development, review |
| **Reporter** (External) | Provide reproduction details; observe embargo; accept credit or anonymity |

## 7. Metrics and KPIs

The following metrics are tracked per incident and reviewed annually:

| Metric | Target |
|---|---|
| Mean time to acknowledge (MTTA) | ≤ 5 business days |
| Mean time to patch (MTTP) — P1 | ≤ 24 hours |
| Mean time to patch (MTTP) — P2 | ≤ 7 days |
| Mean time to patch (MTTP) — P3 | ≤ 30 days |
| Post-incident review completion | 100% for P1/P2 within 30 days |

## 8. Contact

Security incidents must be reported privately via:  
**GitHub Security Advisory:** [github.com/iamfatness/CoreVideo/security/advisories/new](https://github.com/iamfatness/CoreVideo/security/advisories/new)

Do not open public issues for security vulnerabilities.
