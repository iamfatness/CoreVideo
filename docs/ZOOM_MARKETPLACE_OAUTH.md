# Zoom Marketplace OAuth setup

CoreVideo uses Zoom Public Client OAuth + PKCE for user sign-in and the same
Marketplace Public Client ID as the Meeting SDK `publicAppKey` for the helper
process. This keeps OAuth and Meeting SDK secrets out of the OBS plugin while
still supporting attributed joins, external-account meetings, and Marketplace
review.

Published builds use an HTTPS OAuth broker at `corevideo.iamfatness.us`. The OBS
plugin only knows the broker start URL. End users never enter app credentials or
client secrets.

## Zoom Marketplace app (publisher, one-time)

1. Create a **General** app in the Zoom App Marketplace.
2. Enable **User-managed** OAuth.
3. Add this Redirect URL:
   `https://corevideo.iamfatness.us/oauth/callback`
4. Add the same value to the OAuth allow list:
   `https://corevideo.iamfatness.us/oauth/callback`
5. Add the minimum scopes used by the build:
   `user:read:token`
   `user:read:user`
6. Enable **Meeting SDK / Embed** for the same app/environment.
7. Confirm beta or production access is approved for the accounts that will test
   or use the app.

## Broker configuration

The Cloudflare Worker serving `corevideo.iamfatness.us` must have these secrets:

```
ZOOM_OAUTH_PUBLIC_CLIENT_ID=<Marketplace Public Client ID>
ZOOM_OAUTH_AUTHORIZE_URL=https://marketplace.zoom.us/v2/authorize
ZOOM_OAUTH_REDIRECT_URI=https://corevideo.iamfatness.us/oauth/callback
ZOOM_OAUTH_SCOPES=user:read:token user:read:user
COREVIDEO_OAUTH_BROKER_SECRET=<random 32+ byte secret>
```

The broker performs OAuth token exchange only. Meeting SDK auth uses the
public client ID compiled into CoreVideo as `publicAppKey`; no Meeting SDK
client secret is stored by the broker, committed, baked into the OBS plugin, or
entered by end users.

## Embedding the app identity into the build (publisher)

The OAuth broker URL is part of the published app's identity, not a per-user
setting. CoreVideo bakes it in at compile time:

```
cmake -B build \
  -DZOOM_EMBED_OAUTH_CLIENT_ID=y6sIWSwiTZe1JygMx4C9EQ \
  -DZOOM_EMBED_OAUTH_AUTHORIZATION_URL=https://corevideo.iamfatness.us/oauth/start ...
  -DZOOM_EMBED_MEETING_SDK_PUBLIC_APP_KEY=y6sIWSwiTZe1JygMx4C9EQ ...
```

In CI, pass the values as GitHub Actions secrets so they never land in the
source tree. They are written into `src/zoom-credentials.h` from
`src/zoom-credentials.h.in` and read by `ZoomPluginSettings::load()`.

When embedded values are present, they win over OBS `global.ini` so a stale
local config cannot change the published app identity. Developers can still use
`global.ini` overrides only in local builds where the embedded values are blank.

## End-user sign-in

1. Install a CoreVideo build that has the app identity embedded.
2. Open OBS, then open **Tools > Zoom Plugin Settings**.
3. In the **Zoom Account** section click **Sign in with Zoom** and approve the
   app in the browser. There are no Client ID, Client Secret, or Authorization
   URL fields to configure; the build already knows the broker URL.
4. The callback helper (`CoreVideoOAuthCallback.exe` on Windows,
   `CoreVideoOAuthCallback.app` on macOS) is registered for the `corevideo://`
   URL scheme the first time you click Sign in and forwards the redirect to the
   running plugin on `127.0.0.1:<ControlServerPort>` (default `19870`).

## Runtime flow

1. CoreVideo opens the system browser at the embedded broker start URL with a
   local `state` and `return_uri=corevideo://oauth/callback`.
2. The broker generates a PKCE verifier/challenge and redirects to
   `https://marketplace.zoom.us/v2/authorize` with
   `redirect_uri=https://corevideo.iamfatness.us/oauth/callback`.
3. Zoom redirects back to the broker.
4. The broker returns an encrypted, short-lived broker token containing the
   authorization code to `corevideo://oauth/callback`.
5. The callback helper forwards that URL to the running plugin. The plugin
   verifies `state`, redeems the broker token over HTTPS, and the broker
   exchanges the code for access/refresh tokens using Public Client OAuth:
   `client_id` and `code_verifier` in the form body, with no client secret and
   no Authorization header.
6. Before joining a meeting, the plugin refreshes the access token through the
   broker if needed and fetches the signed-in user's ZAK from Zoom.
7. The plugin starts `ZoomObsEngine` with `AuthContext.publicAppKey` set to the
   embedded Marketplace Public Client ID and `AuthContext.jwt_token` set to
   null. The helper joins with the signed-in user's ZAK.
8. The helper uses Zoom's default Meeting SDK window, so the operator can admit
   waiting-room participants, manage self video/audio, and use normal meeting
   controls.

## The enforced join decision path (issue #89)

Sign-in has always worked; meeting *join* kept regressing because the code chose
between OAuth / ZAK / public app key / SDK JWT / broker in several scattered
places. Those choices are now centralized in one pure, unit-tested function,
`zoom_join::plan_join()` in `src/zoom-join-decision.h`. Every caller (the dock's
`on_join_clicked`, the engine client's error mapping, and the OAuth manager's
token-error mapping) reads from that one place.

The production path is fixed:

1. **Sign-in** — Public Client OAuth + PKCE, issued through the HTTPS broker
   (`/oauth/start`). `plan_join` reports `oauth_flow=broker`. Direct
   `public_pkce` is a dev-only fallback when no broker URL is embedded.
2. **ZAK** — if the operator did not supply a ZAK or on-behalf token, the
   signed-in user's ZAK is fetched over OAuth (`zak=fetch_via_oauth`). If no
   OAuth tokens are held yet, the plan sets `sign_in_required=1` and the dock
   starts sign-in and retries the join afterward.
3. **Meeting SDK auth** — the Marketplace Public Client ID is passed as
   `AuthContext.publicAppKey` (`sdk_auth_mode=public_app_key`). Self-signed JWT
   (from an SDK key/secret pair) and broker-minted SDK JWT are dev-only
   fallbacks and are logged loudly as such, never silently.

### The join-decision log block

Every join attempt emits one coherent block (grep `[join-decision]`). It never
contains a full secret — only masked tails (`****WXYZ`), kinds, and flags:

```
[obs-zoom-plugin] [join-decision] oauth_flow=broker oauth_client=****WXYZ \
  broker=corevideo.iamfatness.us sdk_auth_mode=public_app_key \
  public_app_key=****WXYZ zak=fetch_via_oauth token_type=auto_zak \
  have_access_token=1 have_refresh_token=1 token_expired=0 \
  sign_in_required=0 blocking_error=none
```

After the ZAK / broker-JWT step resolves, a second `[join-decision] resolved …`
line records the final auth mode and ZAK presence. The Meeting SDK result code
itself arrives asynchronously from the `ZoomObsEngine` child process and is
logged by the engine client as `auth_ok` or `auth_fail` with the raw
`AUTHRET_*` name and code.

### Error-message catalog

All operator-facing auth/join failures map to one enum, `zoom_join::ZoomJoinError`,
with distinct guidance in `join_error_guidance()`. Distinct categories:

| Category | Trigger (examples) | Operator guidance summary |
| --- | --- | --- |
| `wrong_environment` | OAuth `invalid_client`; `AUTHRET_JWTTOKENWRONG` in public-app-key mode; `AUTHRET_CLIENT_INCOMPATIBLE` | App identity is not valid for this Zoom environment / build. |
| `expired_token` | OAuth `invalid_grant`; expired token, no refresh token | Sign in with Zoom again. |
| `invalid_redirect` | OAuth `redirect_uri_mismatch` | Redirect URI not on the Marketplace allow list (publisher fix). |
| `missing_approval` | OAuth `invalid_scope`; meeting fail 63/60/62/64 | App not published/approved or account not entitled. |
| `sdk_auth_failure` | `AUTHRET_*` key/secret/jwt rejection (JWT mode) | Meeting SDK rejected the identity. |
| `sdk_entitlement` | `AUTHRET_ACCOUNTNOTSUPPORT` / `ACCOUNTNOTENABLESDK` | Account not enabled for the Meeting SDK. |
| `network_issue` | `AUTHRET_NETWORKISSUE` / `OVERTIME` / `SERVICE_BUSY` | Transient network problem; retry. |
| `on_behalf_token_invalid` | meeting fail 500/502/503/505/506 | On-behalf token bad or mismatched. |
| `needs_sign_in` | needs a ZAK but no tokens; meeting fail 504/82/23 | Sign in with Zoom first. |
| `missing_credentials` | no SDK identity embedded and engine not authed | Install a build with the embedded identity. |

These strings surface through the existing dock status/banner + error label and
land in the support bundle, so the raw `AUTHRET_*` code stays available while the
operator sees actionable text. The catalog and the planner are covered by
`tests/join-decision-test.cpp` (ctest target `CoreVideoJoinDecision`), which
also asserts that every category's message is non-empty and distinct and that
the log block never leaks a full identifier.

## Security notes

- No OAuth or Meeting SDK client secret is shipped in the binary. The settings
  dialog does not expose Client ID, Client Secret, or Authorization URL fields
  for published builds, so users cannot misconfigure the integration.
- Broker state is HMAC-signed and expires after 10 minutes. Broker result
  tokens are AES-GCM encrypted, contain only the authorization code, and expire
  after 5 minutes.
- Windows token storage uses DPAPI before writing tokens into OBS global config.
  macOS and Linux have no OS-level secret store wired up yet, so tokens are
  written to OBS global config in plaintext there; the plugin logs a one-time
  `SECURITY:` warning on those platforms. See README.md's Security section.
  Keychain (macOS) / libsecret (Linux) support is tracked as follow-up work.
- Refresh tokens are rotated; always persist the latest refresh token Zoom
  returns.
- Windows builds must ship Qt's TLS backend plugins, especially the Schannel
  backend under `obs-plugins/64bit/plugins/tls`, or OAuth HTTPS requests will
  fail before broker tokens or ZAKs can be fetched.
- The URL callback command bypasses the local control-server token, but the
  OAuth `state` is still required before any broker token can be redeemed.
- Do not log access tokens, refresh tokens, ZAKs, authorization codes,
  broker tokens, OAuth state values, or Meeting SDK secrets.

## Marketplace review checklist

- Explain that CoreVideo joins meetings as an OBS capture/ISO recording tool.
- Request only the scopes used by the build.
- Provide test credentials and a test meeting hosted outside the app account.
- Document the visible in-product OAuth sign-in and uninstall/disconnect path.
- Make sure the Marketplace listing explains when meeting audio/video is
  captured, where it is processed, and that raw media stays local unless OBS
  outputs it.
