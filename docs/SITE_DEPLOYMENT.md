# CoreVideo Site Deployment

The public documentation site is generated from this repository into `public/`
with:

```sh
node scripts/build-site.mjs
```

The live site at `https://corevideo.iamfatness.us/` is expected to be deployed
from `public/` to Cloudflare Pages.

## GitHub Actions Setup

The `Deploy Site` workflow runs on documentation/site changes pushed to `main`
and can also be run manually from the GitHub Actions tab.

Required GitHub repository secrets:

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`

Required GitHub repository variable:

- `CLOUDFLARE_PAGES_PROJECT_NAME`

The API token needs permission to deploy the target Cloudflare Pages project.

## Manual Deploy

If Wrangler is authenticated locally, a manual deploy is:

```sh
node scripts/build-site.mjs
npx wrangler@latest pages deploy public --project-name "$CLOUDFLARE_PAGES_PROJECT_NAME" --branch main
```

After deployment, verify:

```sh
curl -I https://corevideo.iamfatness.us/core-plugin/
curl https://corevideo.iamfatness.us/core-plugin/ | grep "Core Plugin Functionality"
curl https://corevideo.iamfatness.us/documentation/ | grep "Auto ISO Recording"
```
