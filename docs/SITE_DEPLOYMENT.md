# CoreVideo Site Deployment

The public documentation site is generated from this repository into `public/`
with:

```sh
node scripts/build-site.mjs
```

The live site is deployed as static assets on the Cloudflare Worker named
`corevideo-docs`. The primary domain is `https://corevideo.io/`, added as a
Worker custom domain (with `www.corevideo.io` redirecting to the apex). The
original `corevideo.iamfatness.us/*` route continues to serve the same content
as an alias, so both hostnames stay live. Canonical tags on every page point at
`corevideo.io` so it is treated as the primary host.

## corevideo.io prerequisites

Because `corevideo.io` and `www.corevideo.io` are declared as Worker
`custom_domain` routes, the first deploy after this change creates and manages
their proxied DNS records automatically. For that to succeed:

- `corevideo.io` must be an active zone in the same Cloudflare account
  (it is, since the domain was registered through Cloudflare).
- `CLOUDFLARE_API_TOKEN` must have **Workers Scripts: Edit** plus **DNS: Edit**
  and **Zone: Read** on the `corevideo.io` zone (in addition to the existing
  `iamfatness.us` permissions). If the deploy fails attaching the custom domain,
  widen the token.

The OAuth broker endpoints (`/oauth/start`, `/oauth/callback`) served by this
Worker are also reachable on `corevideo.io`, but published plugin builds and the
Zoom Marketplace app still use the `corevideo.iamfatness.us` redirect URI. To
move OAuth to the new domain, add `https://corevideo.io/oauth/callback` to the
Zoom app's redirect URLs and repoint the plugin's broker base URL — that is a
separate change from this website migration.

## GitHub Actions Setup

The `Deploy Site` workflow runs on documentation/site changes pushed to `main`
and can also be run manually from the GitHub Actions tab. It builds `public/`
and runs `npx wrangler@latest deploy`.

Required GitHub repository secrets:

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`

The API token needs permission to deploy Workers scripts and assets for the
target account.

## Manual Deploy

If Wrangler is authenticated locally, a manual deploy is:

```sh
node scripts/build-site.mjs
npx wrangler@latest deploy
```

After deployment, verify:

```sh
curl -I https://corevideo.io/core-plugin/
curl https://corevideo.io/core-plugin/ | grep "Core Plugin Functionality"
curl https://corevideo.io/documentation/ | grep "Auto ISO Recording"

# The alias must keep serving the same content:
curl -I https://corevideo.iamfatness.us/core-plugin/

# www must redirect to the apex:
curl -sI https://www.corevideo.io/ | grep -i location
```
