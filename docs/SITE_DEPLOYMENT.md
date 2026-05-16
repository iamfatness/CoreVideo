# CoreVideo Site Deployment

The public documentation site is generated from this repository into `public/`
with:

```sh
node scripts/build-site.mjs
```

The live site at `https://corevideo.iamfatness.us/` is deployed as static
assets on the existing Cloudflare Worker named `corevideo-docs`. The Cloudflare
route is `corevideo.iamfatness.us/*`.

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
curl -I https://corevideo.iamfatness.us/core-plugin/
curl https://corevideo.iamfatness.us/core-plugin/ | grep "Core Plugin Functionality"
curl https://corevideo.iamfatness.us/documentation/ | grep "Auto ISO Recording"
```
