# Zoom Marketplace OAuth setup

CoreVideo uses a Zoom Meeting SDK JWT to initialize the Meeting SDK helper and
uses a user-level OAuth token to fetch a short-lived ZAK for attributed joins.
This is needed for external-account meetings and Marketplace review.

## Zoom Marketplace app

1. Create a **General** app in the Zoom App Marketplace.
2. Enable **User-managed** OAuth and **PKCE / public client** support.
3. Add this Redirect URL:
   `corevideo://oauth/callback`
4. Add the same value to the OAuth allow list:
   `corevideo://oauth/callback`
5. Add the minimum scopes needed for the first version:
   `user:read:zak`
   `user:read:user`
6. Keep the Meeting SDK credentials configured for SDK authentication. Do not
   embed an OAuth client secret in CoreVideo.

## Local OBS setup

1. Build and install CoreVideo.
2. Open OBS, then open **Tools > Zoom Plugin Settings**.
3. Enter the OAuth Client ID from the Zoom Marketplace app, or paste Zoom's
   test authorization URL into **Authorization URL**. If the URL contains
   `client_id`, CoreVideo can infer and store it.
4. Keep Redirect URI set to `corevideo://oauth/callback`.
5. Click **Register corevideo:// URL Scheme**. On Windows this registers:
   `HKCU\Software\Classes\corevideo\shell\open\command`
   to launch `CoreVideoOAuthCallback.exe`.
   On macOS this registers the bundled `CoreVideoOAuthCallback.app` with
   Launch Services.
6. Click **Authorize with Zoom** and approve the app in the browser.
7. Return to OBS. The callback helper forwards the URL to the plugin over
   `127.0.0.1:19870` using the `oauth_callback` command.

## Runtime flow

1. CoreVideo generates a high-entropy `code_verifier`, derives an S256
   `code_challenge`, starts from Zoom's configured authorization URL if one is
   present, and injects the PKCE parameters it must control.
2. Zoom redirects to `corevideo://oauth/callback?...`.
3. `CoreVideoOAuthCallback.exe` forwards that callback to the running OBS
   plugin. On macOS, `CoreVideoOAuthCallback.app` performs the same forwarding.
   The helper reads the configured CoreVideo control-server port from the OBS
   global config and falls back to `19870`.
4. The plugin verifies `state`, exchanges the authorization code at
   `https://zoom.us/oauth/token`, and stores access/refresh tokens.
5. Before joining, the plugin refreshes the access token if needed and calls:
   `GET https://api.zoom.us/v2/users/me/zak`
6. The returned ZAK is passed into the Meeting SDK `JoinParam4WithoutLogin`
   as `userZAK`.

## Security notes

- PKCE is used because CoreVideo is a desktop plugin and cannot safely store an
  OAuth client secret.
- Windows token storage uses DPAPI before writing tokens into OBS global config.
- Refresh tokens are rotated; always persist the latest refresh token Zoom
  returns.
- Windows builds must ship Qt's TLS backend plugins, especially the Schannel
  backend under `obs-plugins/64bit/plugins/tls`, or OAuth HTTPS requests will
  fail before tokens or ZAKs can be fetched.
- The URL callback command bypasses the local control-server token, but the
  OAuth `state` and one-time verifier are still required before any token
  exchange occurs.
- Do not log access tokens, refresh tokens, ZAKs, authorization codes, or PKCE
  verifiers.

## Marketplace review checklist

- Explain that CoreVideo joins meetings as an OBS capture/ISO recording tool.
- Request only the scopes used by the build.
- Provide test credentials and a test meeting hosted outside the app account.
- Document the visible in-product OAuth sign-in and uninstall/disconnect path.
- Make sure the Marketplace listing explains when meeting audio/video is
  captured, where it is processed, and that raw media stays local unless OBS
  outputs it.
