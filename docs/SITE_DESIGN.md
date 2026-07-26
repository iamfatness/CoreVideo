# CoreVideo Site Design System

The public site (`corevideo.io`) uses the same **broadcast-console design
language** as the CoreVideo Pro desktop app, so the two read as one product.
The app's canonical tokens live in
`native-shell/CoreVideoPro.WinUI/Themes/StudioTheme.xaml` (in the CoreVideoPro
repo); the site mirrors them.

## Where it lives

- **`site-assets/site.css`** — the single source of truth for the site's design
  system (tokens, components, layout). `scripts/build-site.mjs` copies it verbatim
  to `public/assets/site.css`. Edit the design here, not in the generated output.
- **`site-assets/fonts/*.woff2`** — self-hosted, Latin-subset app fonts, copied to
  `public/assets/fonts/`. Served under CSP `font-src 'self'`.
- Brand mark + favicon are generated in `build-site.mjs` (`MULTIVIEW_MARK`,
  `favicon.svg`).

## Tokens (mirror StudioTheme.xaml)

| Role | Value |
|---|---|
| Background | `#0a0b0c` |
| Panel / card | `#101315` |
| Surface (hover) | `#16191b` |
| Field / screen inset | `#0e1112` |
| Hairline border | `rgba(255,255,255,0.09)` |
| Primary text | `#e9edef` |
| Muted text | `#8b949b` |
| Dim (mono labels) | `#5c656b` |
| **Accent (LIVE / brand)** | `#22c86e` on ink `#06170d` |
| Program / warning (amber) | `#e8a41f` |
| On-air / record (red) | `#e5433f` |

Radius 8px default (12px cards). No drop shadows — elevation is tonal
(surface-color steps + hairline borders), matching the app.

## Type

- **Space Grotesk** (variable, wght 300–700) — UI/display.
- **IBM Plex Mono** (400/500/600) — telemetry: nav, eyebrow labels, table
  headers, tally chips, timecodes, footer. Authored uppercase with wide tracking.

## Signature idioms

- **Multiview brand mark** — 2×2 monitor grid with the top-right tile live-green
  (mirrors `Controls/MultiviewMark.xaml`). Header logo + favicon.
- **Tally chips** — `.tally-live` (green), `.tally-pgm` (amber), `.tally-air`
  (red), with pulsing dots — the app's broadcast state semantics.
- **Console frame** — hero media is wrapped like an operator monitor: a mono
  bezel bar (tallies + resolution + timecode) over a `--field` screen.
- **Featured card** — inset green accent rail, like the app's program tally bar.
- **Mono eyebrow** — green mono uppercase label with a pulsing live dot.

## Changing the design

Edit `site-assets/site.css`, run `node scripts/build-site.mjs`, and commit both
the source and regenerated `public/`. To refresh fonts, re-subset from the app's
`Assets/Fonts/*.ttf` with `python -m fontTools.subset ... --flavor=woff2`.
