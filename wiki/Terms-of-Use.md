# Terms of Use

_Last updated: May 2026_

## 1. Acceptance

By installing, configuring, or using CoreVideo ("the Plugin"), you agree to these Terms of Use and to all applicable third-party terms referenced herein. If you do not agree, do not use the Plugin.

---

## 2. License

CoreVideo is released under the terms of its open-source license (see [LICENSE](https://github.com/iamfatness/CoreVideo/blob/main/LICENSE) in the repository). You are free to use, modify, and redistribute the Plugin in accordance with that license.

---

## 3. Zoom Meeting SDK Raw Data and Account Limits

CoreVideo uses the Zoom Meeting SDK's **raw data APIs** to capture uncompressed video and audio. Raw data access is available through Meeting SDK apps, but negotiated quality, bandwidth, meeting duration, and concurrent stream count are governed by the signed-in Zoom account, the meeting configuration, and app/developer entitlements.

Enhanced Media / HBM is not a hard prerequisite for raw data access. It can materially increase production headroom by raising the incoming video bandwidth envelope from roughly 30 Mbps to roughly 100 Mbps. At about 4-6 Mbps per standard 1080p stream, several 1080p feeds may work without EM; with EM/HBM, plan around up to 16 standard 1080p feeds or about 8 high-bitrate / 60 fps feeds before hitting the downlink budget.

By using CoreVideo you confirm that:

- You are responsible for confirming the Zoom account, meeting, and app entitlements needed for your intended production quality and stream count.
- You will not use the Plugin to capture Zoom meeting content in violation of the [Zoom Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement) or applicable Zoom terms of service.
- You understand that account bandwidth, app approval, developer flags, and Zoom plan limits can affect the number and quality of raw streams CoreVideo can receive.

---

## 4. Zoom Terms of Service

Your use of the Zoom Meeting SDK through CoreVideo is additionally governed by:

- [Zoom Terms of Service](https://explore.zoom.us/en/terms/)
- [Zoom Marketplace Developer Agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement)
- [Zoom Privacy Policy](https://explore.zoom.us/en/privacy/)

CoreVideo is an independent project and is **not affiliated with, endorsed by, or supported by Zoom Video Communications, Inc.**

---

## 5. Meeting Participant Consent

You are solely responsible for ensuring that all meeting participants have been informed of, and have consented to, any recording or live capture of their video, audio, or screen-share content. Requirements vary by jurisdiction. Consult legal counsel if unsure.

---

## 6. Permitted Use

The Plugin may be used for:

- Professional broadcast, live production, and event streaming
- Conference and event recording (with appropriate consent)
- Internal corporate communications and hybrid events
- Educational and research purposes

The Plugin may **not** be used for:

- Unauthorized recording or surveillance of meeting participants
- Capturing content in violation of applicable wiretapping, recording, or privacy laws
- Any purpose that violates Zoom's terms of service or the Zoom Marketplace Developer Agreement

---

## 7. No Warranty

The Plugin is provided **"as is"**, without warranty of any kind, express or implied, including but not limited to warranties of merchantability, fitness for a particular purpose, or non-infringement. The authors make no guarantee that the Plugin will operate without interruption or error.

---

## 8. Limitation of Liability

To the maximum extent permitted by applicable law, the authors of CoreVideo shall not be liable for any indirect, incidental, special, consequential, or punitive damages arising from your use of or inability to use the Plugin, even if advised of the possibility of such damages.

---

## 9. Changes

These terms may be updated at any time. Continued use of the Plugin after changes are posted constitutes acceptance of the revised terms. Material changes will be noted in the [release notes](https://github.com/iamfatness/CoreVideo/releases).

---

## 10. Contact

Questions about these terms may be directed to [github.com/iamfatness/CoreVideo/issues](https://github.com/iamfatness/CoreVideo/issues).
