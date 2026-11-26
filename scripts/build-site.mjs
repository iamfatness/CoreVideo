import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const wikiDir = path.join(root, "wiki");
const docsDir = path.join(root, "docs");
const siteAssetsDir = path.join(root, "site-assets");
const outDir = path.join(root, "public");

// corevideo.io is the primary domain; corevideo.iamfatness.us continues to serve
// the same content as an alias. Canonical tags point search engines at the
// primary host so the two domains are not treated as duplicate content.
const PRIMARY_ORIGIN = "https://corevideo.io";
const SITE_NAME = "CoreVideo";
// Cloudflare Web Analytics (cookieless). The zone's auto-install never reached
// the pages: the worker's CSP blocked the injected beacon, so the dashboard
// stayed empty. Emitted explicitly here, with site-worker.js allowing the two
// Cloudflare hosts. The token is public by design (it ships in every page).
const ANALYTICS_BEACON =
  `  <script defer src="https://static.cloudflareinsights.com/beacon.min.js" data-cf-beacon='{"token": "606f185a54e7489bb11a93235343469b"}'></script>\n`;
// Initialize the data layer before loading the container. The site's CSP
// requires the initializer to be served as a separate script.
const GOOGLE_TAG_MANAGER = `  <script src="/assets/google-tag-manager.js"></script>
  <script async src="https://www.googletagmanager.com/gtm.js?id=GTM-PSHZHBGZ"></script>`;
const OG_IMAGE = `${PRIMARY_ORIGIN}/assets/corevideo-share.jpg`;
const OG_IMAGE_ALT =
  "CoreVideo - Zoom participant video and audio as native OBS Studio sources";

// Every canonical URL is, by definition, a page we want indexed: aliases and
// redirect targets never call canonicalUrl(), so this set is exactly the
// sitemap. Pages that build their own <head> (the documentation page) register
// themselves explicitly.
const indexableUrls = new Set();

function canonicalUrl(output) {
  const pathPart = ("/" + output).replace(/index\.html$/, "");
  const url = PRIMARY_ORIGIN + (pathPart || "/");
  indexableUrls.add(url);
  return url;
}

// The project() version in CMakeLists.txt is a documented development
// placeholder - the real version comes from the release tag, and CHANGELOG.md
// is the one in-repo file that tracks it. Release builds can override.
function latestReleaseVersion() {
  const override = process.env.COREVIDEO_RELEASE_VERSION?.trim().replace(/^v/, "");
  if (override) return override;
  const changelogPath = path.join(root, "CHANGELOG.md");
  if (fs.existsSync(changelogPath)) {
    const match = fs
      .readFileSync(changelogPath, "utf8")
      .match(/^## \[(\d+\.\d+\.\d+)\]/m);
    if (match) return match[1];
  }
  // Never print a guessed version. The download page drops its version-stamped
  // links and offers only the always-current release link instead.
  console.warn(
    "Warning: no release version found (CHANGELOG.md heading or " +
      "COREVIDEO_RELEASE_VERSION); the download page will link to " +
      "releases/latest only.",
  );
  return null;
}

const RELEASE_VERSION = latestReleaseVersion();
// Keep the Apple Silicon pin independent for Windows-only releases.
const MAC_VERSION = "0.1.55";
const MAC_RELEASE = `https://github.com/iamfatness/CoreVideo/releases/tag/v${MAC_VERSION}`;
const MAC_INSTALLER = `https://github.com/iamfatness/CoreVideo/releases/download/v${MAC_VERSION}/CoreVideo-Setup-v${MAC_VERSION}.pkg`;
const RELEASES_LATEST = "https://github.com/iamfatness/CoreVideo/releases/latest";

// ZComms ships from its own repository, so this site cannot read its version
// from a file the way CoreVideo's comes out of CHANGELOG.md. The constant is
// printed as prose only - every ZComms download link points at releases/latest,
// so a stale constant reads as an out-of-date sentence and never as a 404.
// Bump it (or set ZCOMMS_RELEASE_VERSION) when ZComms cuts a release.
const ZCOMMS_VERSION =
  process.env.ZCOMMS_RELEASE_VERSION?.trim().replace(/^v/, "") || "0.1.14";
const ZCOMMS_REPO = "https://github.com/iamfatness/ZComms";
const ZCOMMS_RELEASES_LATEST = `${ZCOMMS_REPO}/releases/latest`;

// CoreVideo Pro also ships from its own repository. 1.0.0 (2026-11-12) is the
// first stable release. The announcement pins the 1.0.0 installer on purpose:
// it is the release being announced, and that asset never moves. Every other
// Pro download button points at releases/latest so a later 1.0.x is what
// people get.
const PRO_VERSION = "1.0.0";
const PRO_REPO = "https://github.com/iamfatness/CoreVideoPro";
const PRO_RELEASES_LATEST = `${PRO_REPO}/releases/latest`;
const PRO_RELEASE_TAG = `${PRO_REPO}/releases/tag/v${PRO_VERSION}`;
const PRO_SETUP = `${PRO_REPO}/releases/download/v${PRO_VERSION}/CoreVideoPro-Setup-v${PRO_VERSION}.exe`;
const PRO_ANNOUNCEMENT = "/news/corevideo-pro-1-0/";

// Pro 1.0 on macOS with eight generated guest feeds in a sample show.
const PRO_STUDIO_IMAGE = {
  file: "pro-studio-1-0.webp",
  width: 1920,
  height: 1254,
  alt: "CoreVideo Pro Studio with a host in Preview, a six-person panel in Program, and eight source tiles below",
};

// ZComms: the v0.1.14 panel rendered with a sample roster and channel state.
const ZCOMMS_PANEL_IMAGE = {
  file: "zcomms-talkback-0-1-14.webp",
  width: 2000,
  height: 1280,
  alt: "ZComms talkback panel showing All Call, named talk keys, an active on-air key, and the operator microphone meter",
};

// GitHub's releases/latest excludes prereleases. Keep Edge's beta tag explicit.
const EDGE_REPO = "https://github.com/iamfatness/CoreVideoEdge";
const EDGE_BETA = "beta-2026-11-11-0d83fc2";
const EDGE_RELEASE = `${EDGE_REPO}/releases/tag/${EDGE_BETA}`;
const EDGE_DOWNLOADS = `${EDGE_REPO}/releases/download/${EDGE_BETA}`;

const VOLT_REPO = "https://github.com/iamfatness/CoreVideo-Volt";
const VOLT_OPERATIONS = `${VOLT_REPO}/blob/HEAD/docs/operations.md`;

// The CoreVideo "Multiview" brand mark (mirrors Controls/MultiviewMark.xaml in
// the app): a 16:9 monitor frame split 2x2 with the top-right tile live-green.
const MULTIVIEW_MARK =
  `<svg class="mv" viewBox="0 0 32 24" width="28" height="21" fill="none" aria-hidden="true">` +
  `<rect x="1" y="1" width="30" height="22" rx="4" stroke="currentColor" stroke-width="1.6"/>` +
  `<line x1="16" y1="2" x2="16" y2="22" stroke="currentColor" stroke-width="1.2" opacity=".45"/>` +
  `<line x1="2" y1="12" x2="30" y2="12" stroke="currentColor" stroke-width="1.2" opacity=".45"/>` +
  `<rect x="17.4" y="2.6" width="12.8" height="8.6" rx="1.6" fill="#22c86e"/></svg>`;

const pages = [
  {
    source: "Home.md",
    title: "CoreVideo",
    seoTitle: "CoreVideo - Production Tools for Live Zoom Shows",
    description:
      "Production tools for Zoom shows: the CoreVideo OBS plugin, CoreVideo Pro, Edge, ZComms, and Volt for routing, switching, talkback, recording, and publishing.",
    output: "index.html",
  },
  {
    source: "Terms-of-Use.md",
    title: "Terms of Use",
    description:
      "Terms of Use for the CoreVideo OBS Studio plugin: licensing, Zoom Meeting SDK limits, participant consent, talkback, and acceptable use.",
    output: "terms/index.html",
    aliases: ["terms-of-use/index.html", "Terms-of-Use/index.html"],
  },
  {
    source: "Privacy-Policy.md",
    title: "Privacy Policy",
    description:
      "Privacy Policy for CoreVideo: what the OBS plugin processes locally on your machine, what is never uploaded, and which third-party services are involved.",
    output: "privacy/index.html",
    aliases: ["privacy-policy/index.html", "Privacy-Policy/index.html"],
  },
  {
    source: "Support.md",
    title: "Support",
    description:
      "Get help with CoreVideo: troubleshooting steps, log collection, known issues, and how to report a bug against the OBS Studio plugin.",
    output: "support/index.html",
    aliases: ["Support/index.html"],
  },
];

const markdownPages = [
  {
    source: path.join(docsDir, "CORE_PLUGIN_FUNCTIONALITY.md"),
    title: "CoreVideo (OBS Plugin)",
    seoTitle: "CoreVideo OBS Plugin Guide - Zoom ISO Recording & Routing",
    description:
      "How to route Zoom participants to OBS sources: per-participant video and audio, active-speaker follow, spotlight slots, screen share, and ISO recording.",
    output: "core-plugin/index.html",
  },
  {
    source: path.join(docsDir, "OPERATOR_QUICKSTART.md"),
    title: "Operator Quickstart",
    seoTitle: "CoreVideo Operator Quickstart - Zoom in OBS",
    description: "Install CoreVideo, sign in to Zoom, start the engine, and assign your first participant source in OBS.",
    output: "quickstart/index.html",
  },
];

const publicDocumentationUrl =
  process.env.COREVIDEO_SITE_URL?.replace(/\/$/, "") || "";

function ensureDir(filePath) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
}

function escapeHtml(value) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function inlineMarkdown(value) {
  const code = [];
  const protectedValue = value.replace(/`([^`]+)`/g, (_match, raw) => {
    const index = code.push(`<code>${escapeHtml(raw)}</code>`) - 1;
    return `@@CODE${index}@@`;
  });
  return escapeHtml(protectedValue)
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/_([^_]+)_/g, "<em>$1</em>")
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_match, label, href) => {
      const resolved = resolveHref(href);
      return `<a href="${escapeHtml(resolved)}">${label}</a>`;
    })
    .replace(/@@CODE(\d+)@@/g, (_match, index) => code[Number(index)] ?? "");
}

function renderImage(alt, src) {
  return `<figure class="doc-image"><img src="${escapeHtml(src)}" alt="${escapeHtml(alt)}"></figure>`;
}

function resolveHref(href) {
  if (href.startsWith("https://iamfatness.github.io/CoreVideo/#"))
    return `/documentation/#${href.slice("https://iamfatness.github.io/CoreVideo/#".length)}`;
  if (href.startsWith("https://corevideo.io/documentation/#"))
    return `/documentation/#${href.slice("https://corevideo.io/documentation/#".length)}`;
  if (href === "https://corevideo.io/documentation/")
    return "/documentation/";
  if (href.startsWith("https://corevideo.iamfatness.us/documentation/#"))
    return `/documentation/#${href.slice("https://corevideo.iamfatness.us/documentation/#".length)}`;
  if (href === "https://corevideo.iamfatness.us/documentation/")
    return "/documentation/";
  if (href === "https://iamfatness.github.io/CoreVideo/")
    return "/documentation/";
  if (href === "Privacy-Policy")
    return "/privacy/";
  if (href === "Terms-of-Use")
    return "/terms/";
  if (href === "Support")
    return "/support/";
  return href;
}

// Replaces Windows-1252 mojibake sequences that appear when UTF-8 wiki content
// is accidentally re-encoded. Using \u escapes to prevent corruption in transit.
function normalizeText(value) {
  return value
    .replaceAll("â€”", "-")
    .replaceAll("â†’", "->")
    .replaceAll("â€¦", "...")
    .replaceAll("ðŸ“–", "")
    .replaceAll("behaviour", "behavior")
    .replaceAll("https://corevideo.io/documentation/", "/documentation/")
    .replaceAll("https://corevideo.iamfatness.us/documentation/", "/documentation/")
    .replaceAll("https://iamfatness.github.io/CoreVideo/", "/documentation/")
    .replaceAll("https://iamfatness.github.io/CoreVideo", "/documentation");
}

// The one-line strip that opens the home page while a release is news. It is a
// link, not a modal or a cookie-gated banner: one sentence and a destination.
const products = [
  { name: "CoreVideo for OBS", path: "/obs/", platform: "Windows and Apple Silicon", description: "Bring Zoom guests into OBS as individual video and audio sources." },
  { name: "CoreVideo Pro", path: "/pro/", platform: "Windows", description: "Switch scenes, record, and stream from a standalone production studio." },
  { name: "CoreVideo Edge", path: "/edge/", platform: "Windows beta", description: "Route and record additional Zoom guests on a second machine." },
  { name: "ZComms", path: "/zcomms/", platform: "Windows", description: "Cue talent privately from a dedicated talkback and IFB station." },
  { name: "Volt", path: "/volt/", platform: "Source installation", description: "Run the rundown, cue clips, and publish from one show session." },
];

function homeContent() {
  return `<section class="suite-hero">
  <div class="suite-intro">
    <h1>Make Zoom the core of your show.</h1>
    <p class="lede">Choose the tools you need to route guests, switch the show, cue talent, and deliver the recording.</p>
    <div class="hero-actions">
      <a class="button primary" href="#products">Explore products</a>
      <a class="button" href="/download/">Get the OBS plugin</a>
    </div>
  </div>
  <figure class="suite-media">
    <img src="/assets/dynamic-gallery.webp" width="1400" height="776" fetchpriority="high" alt="Six Zoom guests composed from individual camera feeds in OBS">
    <figcaption><span class="preview-label">Program preview</span><a href="/obs/">Made with CoreVideo for OBS</a></figcaption>
  </figure>
</section>
<section class="suite-products" id="products" aria-labelledby="products-heading">
  <h2 id="products-heading">Choose your CoreVideo tools</h2>
  <div class="product-grid">
    ${products.map(({name, path, platform, description}) => `<a class="product-tile" href="${path}">
      <span class="product-platform">${platform}</span>
      <h3>${name}</h3>
      <p>${description}</p>
      <span class="link-arrow" aria-hidden="true">&nearr;</span>
    </a>`).join("\n    ")}
  </div>
</section>
<section class="start-panel" aria-labelledby="start-heading">
  <div>
    <h2 id="start-heading">Start with OBS</h2>
    <p>Install the free plugin and route your first Zoom guest in OBS Studio.</p>
  </div>
  <div class="hero-actions">
    <a class="button primary" href="/download/">Download the plugin</a>
    <a class="button" href="/quickstart/">Operator quickstart</a>
  </div>
</section>`;
}

function obsPageContent() {
  return `<section class="hero product-hero">
  <div class="hero-copy">
    <p class="eyebrow">Free, open-source OBS Studio plugin</p>
    <h1>CoreVideo for OBS</h1>
    <p class="lede">Bring Zoom participants into OBS as native video and audio sources. Build scenes from individual guests, screen shares, and interpretation feeds.</p>
    <div class="hero-actions">
      <a class="button primary" href="/download/">Download the plugin</a>
      <a class="button" href="/quickstart/">Operator quickstart</a>
    </div>
  </div>
  <figure class="hero-media product-shot">
    <img src="/assets/obs-multiview.webp" width="1600" height="664" alt="OBS multiview with Preview, Program, and four scenes composed from individual Zoom guests">
    <figcaption>Preview, Program, and scenes in OBS Studio</figcaption>
  </figure>
</section>
<section class="link-grid product-features" aria-label="What the OBS plugin does">
  <div><strong>Assign guests to OBS sources</strong><span>Route a named participant, follow the active speaker, or assign a Zoom spotlight slot. Arrange multiple guests into a gallery with CoreVideo Tiles.</span></div>
  <div><strong>Route screen shares and isolated audio</strong><span>Add a dedicated source for the meeting&rsquo;s screen share or an interpretation channel. Mix each guest on their own audio track.</span></div>
  <div><strong>Record individual feeds</strong><span>Record selected feeds as per-participant MP4s with isolated audio alongside your OBS Program recording.</span></div>
</section>
<section aria-labelledby="obs-guides">
  <h2 id="obs-guides">Set up and operate</h2>
  <div class="link-grid product-features">
    <a href="/core-plugin/"><strong>Core plugin guide</strong><span>Assignments, audio isolation, and recording workflows.</span></a>
    <a href="/documentation/"><strong>OBS plugin documentation</strong><span>Installation, configuration, and control APIs.</span></a>
    <a href="https://github.com/iamfatness/CoreVideo"><strong>GitHub repository</strong><span>Source code, issues, and release history.</span></a>
  </div>
</section>`;
}

function productScreenshot({ file, width, height, alt }, caption) {
  const src = `/assets/${file}`;
  return `<figure class="hero-media product-shot">
    <img src="${src}" width="${width}" height="${height}" alt="${escapeHtml(alt)}">
    <figcaption><span>${caption}</span><a href="${src}">View full size</a></figcaption>
  </figure>`;
}

function screenshotSocialImage({ file, width, height, alt }) {
  return {
    image: `${PRIMARY_ORIGIN}/assets/${file}`,
    imageAlt: alt,
    imageWidth: width,
    imageHeight: height,
  };
}

function proPageContent() {
  return `<section class="hero product-hero screenshot-hero">
  <div class="hero-copy">
    <p class="eyebrow">Standalone production studio for Windows</p>
    <h1>CoreVideo Pro</h1>
    <p class="lede">Switch a Zoom production from one console. Route participant video and audio, cue talent privately, and send Program to recording, streaming, NDI, and a Windows virtual camera.</p>
    <div class="hero-actions">
      <a class="button primary" href="${PRO_RELEASES_LATEST}">Download for Windows</a>
      <a class="button" href="/pro/documentation/">Pro documentation</a>
    </div>
    <p class="product-utility"><a href="${PRO_ANNOUNCEMENT}">What&rsquo;s in 1.0</a><a href="${PRO_REPO}">GitHub repository</a></p>
  </div>
  ${productScreenshot(PRO_STUDIO_IMAGE, "Studio on macOS with eight AI-generated guest feeds in a sample show")}
</section>
<section class="link-grid" aria-label="CoreVideo Pro features">
  <div><strong>Individual Zoom sources</strong><span>Join by link and route each guest&rsquo;s video and audio separately. View the live roster and follow the active speaker as needed.</span></div>
  <div><strong>Talkback inside the meeting</strong><span>The Comms page talks to people in the meeting over Zoom&rsquo;s private talkback channels. The room does not hear you, and there is no second app or audio cable.</span></div>
  <div><strong>1080p60 on the GPU</strong><span>A Direct3D 11 compositor renders Program, Preview and the multiviewer, with per-source framing, borders, colour grading, lower thirds and graphics.</span></div>
  <div><strong>Every output at once</strong><span>Program recording, per-guest ISO recordings, an SRT or RTMP stream, NDI with an optional graphics key, and the &ldquo;CoreVideo Pro Camera&rdquo; for Zoom, Teams or OBS.</span></div>
</section>
<section>
  <h2>What ships in 1.0</h2>
  <div class="feature-set">
    <div>
      <h3>Sources</h3>
      <ul>
        <li>Zoom participants as separate 1080p video and audio sources</li>
        <li>Webcams and capture cards</li>
        <li>Screen and window capture</li>
        <li>Local video clips and stills</li>
      </ul>
    </div>
    <div>
      <h3>Production</h3>
      <ul>
        <li>Scenes with Preview, Program and Take</li>
        <li>CoreVideo Tiles: guests composed into one styled gallery</li>
        <li>A multiviewer with Program and Preview tally</li>
        <li>Graphics playout with a built-in designer, and lower thirds</li>
      </ul>
    </div>
    <div>
      <h3>Comms</h3>
      <ul>
        <li>Talk privately to people in your Zoom meeting</li>
        <li>Private comms mode, so the room never hears you</li>
        <li>Needs host or co-host, and talent on the Zoom desktop or mobile app</li>
      </ul>
    </div>
    <div>
      <h3>Audio</h3>
      <ul>
        <li>Routing matrix with Program and per-guest ISO taps</li>
        <li>BS.1770 loudness metering</li>
        <li>Bus dynamics and a limiter</li>
        <li>Monitor output to any Windows audio device</li>
      </ul>
    </div>
    <div>
      <h3>Recording &amp; streaming</h3>
      <ul>
        <li>Program and per-guest ISO MP4s on hardware encoders</li>
        <li>SRT with GPU-direct HEVC or H.264, and RTMP</li>
        <li>NDI output on its own switch, plus an NDI graphics key (fill and key)</li>
        <li>Recording resumes on its own if the media engine restarts</li>
      </ul>
    </div>
    <div>
      <h3>Control</h3>
      <ul>
        <li>Keyboard and Stream Deck control through a Bitfocus Companion module</li>
        <li>A local control API for automation</li>
        <li>Redacted support bundles for troubleshooting</li>
      </ul>
    </div>
  </div>
</section>
<section>
  <h2>Before you install</h2>
  <ul>
    <li>Windows 10 or later, 64-bit, with a Direct3D 11 GPU and current graphics drivers.</li>
    <li>The installer is <strong>not yet code-signed</strong>, so Windows SmartScreen may warn the first time. Choose <em>More info &rarr; Run anyway</em>. A signed build will follow in a 1.0.x update.</li>
    <li>Setup asks for administrator approval once, to register the virtual camera.</li>
    <li>First launch downloads and verifies the media runtime, so it needs internet access.</li>
  </ul>
  <p>Not in 1.0: macOS, captions, the automatic scene director, and Blackmagic or AJA capture. They are not on this page because they do not ship.</p>
</section>
<section>
  <h2>Which one do I use?</h2>
  <p>Already run your shows in OBS? The free <a href="/core-plugin/">OBS plugin</a> brings clean Zoom participants into the OBS you already have. Need only talkback, with its own operator? <a href="/zcomms/">ZComms</a> is the standalone desk. Want the whole studio in one app? That is CoreVideo Pro.</p>
  <table>
    <thead><tr><th>Product</th><th>Form factor</th><th>Best for</th></tr></thead>
    <tbody>
      <tr><td><strong>CoreVideo</strong> &mdash; Free, available now</td><td>OBS Studio plugin</td><td>Operators already running shows in OBS who want clean Zoom participants as native sources, ISO recording, an Active Speaker Director, and Stream Deck control.</td></tr>
      <tr><td><strong>ZComms</strong> &mdash; Available now</td><td>Standalone Windows app</td><td>Productions that need talkback: named keys into Zoom&apos;s private talkback channels, with its own operator position. See <a href="/zcomms/">ZComms</a>.</td></tr>
      <tr><td><strong>CoreVideo Pro</strong> &mdash; 1.0, available now</td><td>Standalone Windows app</td><td>Producers who want a dedicated studio: Zoom guests, cameras, screens and media in scenes, talkback, and every output from one console.</td></tr>
    </tbody>
  </table>
</section>`;
}

// As built for 1.0, from the CoreVideoPro README's architecture section. This
// page used to be the June design document (a React/Vite renderer in an
// Electron or Tauri shell, macOS, an AI director, Blackmagic/AJA); none of
// that is what shipped, so it now describes the product people can install.
function proDocsContent() {
  return `<h1>CoreVideo Pro Architecture</h1>
<p>How CoreVideo Pro ${PRO_VERSION} is put together on Windows. The as-built detail, with the evidence behind each capability, lives in the <a href="${PRO_REPO}#readme">project README</a>. For the OBS plugin&apos;s internals, see the <a href="/core-plugin/">Core Plugin guide</a>.</p>
<h2>Three processes</h2>
<ul>
<li><strong>The operator console</strong> is a native WinUI 3 app on .NET 9. It owns no real-time media: it sends production commands and draws the program, preview and multiviewer from shared GPU textures.</li>
<li><strong>The media core</strong> is a C++ process that owns real-time pixels and audio: the Direct3D 11 compositor, the audio mixer and dynamics, the Media Foundation recorder, and every output sender.</li>
<li><strong>The Zoom engine</strong> is a separate process that runs the Zoom Meeting SDK and writes each participant&apos;s raw video and audio to shared memory, so a problem inside the SDK cannot take the console down with it.</li>
</ul>
<p>The console and the core talk over typed JSON-line commands and snapshots on the child process&apos;s standard input and output. Video crosses process boundaries as keyed-mutex DXGI shared textures, never as copies through a pipe.</p>
<h2>Sources</h2>
<p>Zoom participants arrive as separate raw 1080p video and audio sources. Webcams and capture cards are read by Media Foundation inside the core, screens and windows through Windows Graphics Capture, and local clips and stills play from a media bin with cue, live and pause owned by the core. Each source keeps its own clock and texture, and scenes select from them.</p>
<h2>Program</h2>
<p>A scene is a layout of slots. The operator builds the next shot in Preview and Takes it to Program. The compositor renders Program, Preview and the multiviewer on the GPU at 1080p60, with per-source framing, borders, colour grading, CoreVideo Tiles, lower thirds and graphics. A short Program buffer (two or three frames) gives every output a steady 60 fps cadence.</p>
<h2>Outputs</h2>
<p>The same Program feeds every output at once: an MP4 Program recording and per-guest ISO recordings on hardware encoders with isolated write queues, an SRT stream with GPU-direct HEVC or H.264 encoding or an RTMP stream, NDI with an optional graphics key, and the &ldquo;CoreVideo Pro Camera&rdquo; registered with the Windows Frame Server for Zoom, Teams or OBS. If the media core restarts, recording resumes and the stream reconnects on their own.</p>
<h2>Comms</h2>
<p>Talkback rides the Zoom Meeting SDK&apos;s private talkback channels, so the operator speaks to chosen people in the meeting without the room hearing. Private comms keeps the operator&apos;s own meeting audio silent.</p>
<h2>How a release is qualified</h2>
<p>Before a release, the installed build runs a 30-minute outputs soak in which every output is judged frame by frame by a picture identity stamped into Program, alongside a clean-machine install test. The ${PRO_VERSION} results are in the <a href="${PRO_RELEASE_TAG}">release notes</a>.</p>`;
}

// The CoreVideo Pro 1.0 announcement. Leads with the four pillars the owner
// chose (Zoom-first, talkback inside the meeting, 1080p60 GPU pipeline with
// every output at once, per-guest ISO plus reliability), never with "native
// Zoom": every serious competitor has a Zoom integration. Qualification is
// reported as measured, including the stream frames skipped under game load.
function proAnnouncementContent() {
  return `<p class="eyebrow">Release &middot; November 12, 2026</p>
<h1>CoreVideo Pro 1.0 is here</h1>
<p class="lede">CoreVideo Pro, the standalone production studio for Zoom shows, has its first stable release. Version ${PRO_VERSION} is available today for Windows.</p>
<div class="hero-actions">
  <a class="button primary" href="${PRO_SETUP}">Download CoreVideo Pro ${PRO_VERSION} for Windows</a>
  <a class="button" href="${PRO_RELEASE_TAG}">Release notes and checksums</a>
  <a class="button" href="/pro/">About CoreVideo Pro</a>
</div>
<h2>Built around the guests, not around a capture window</h2>
<p>Most tools can get a Zoom call on screen. The question is what happens next: when the seventh guest joins, when someone drops and comes back, when you need to tell one panelist to wrap up without the room hearing. CoreVideo Pro starts from the meeting. Every participant arrives through the Zoom Meeting SDK as their own raw 1080p video and audio source, the live roster and active speaker sit beside them, and the show is built in scenes you cue in Preview and Take to Program.</p>
<h2>Talkback, inside the meeting</h2>
<p>New since the last beta, the <strong>Comms</strong> page puts a talkback station into Pro. Talk to the people you choose over Zoom&rsquo;s private talkback channels: they hear you, the room and the recording do not. Turn on Private comms and your own meeting audio stays silent. Comms needs Pro&rsquo;s Zoom account to be host or co-host, and talent on the Zoom desktop or mobile app.</p>
<h2>One Program, every output at once</h2>
<p>The compositor runs on the GPU at 1080p60, with framing, borders, colour grading, CoreVideo Tiles, lower thirds and graphics from a built-in designer. The same Program goes out as:</p>
<ul>
<li>a Program recording and per-guest ISO recordings for the edit, on hardware encoders;</li>
<li>an SRT stream with GPU-direct encoding, or RTMP;</li>
<li>NDI on its own switch, plus an NDI graphics key for a downstream switcher;</li>
<li>the &ldquo;CoreVideo Pro Camera&rdquo;, a Windows virtual camera for Zoom, Teams or OBS.</li>
</ul>
<h2>Tested the way it will be used</h2>
<p>Before release, the installed 1.0 build ran 30-minute output soaks in which every frame of every output is checked against a picture identity stamped into Program. A separate test installed, launched and uninstalled the exact release files on a fresh Windows machine.</p>
<ul>
<li>The Program recording delivered every frame, with none missing or repeated.</li>
<li>The virtual camera delivered every frame in the first soak. In a second soak it delivered one frame out of order.</li>
<li>The stream skipped 12 single frames out of about 108,000 in the first soak and 10 in the second.</li>
</ul>
<p>Both soaks ran while the same PC was also running a game. Earlier soaks of the same test on an idle machine delivered every frame on all three outputs. Frame skips under heavy competing GPU load are tracked for a 1.0.x update.</p>
<h2>Install</h2>
<ol>
<li>Download <a href="${PRO_SETUP}">CoreVideoPro-Setup-v${PRO_VERSION}.exe</a> and run it.</li>
<li>The installer is <strong>not yet code-signed</strong>. If Windows SmartScreen warns, choose <em>More info &rarr; Run anyway</em>. A signed build will follow in a 1.0.x update.</li>
<li>Approve the administrator prompt. It registers the CoreVideo Pro Camera.</li>
<li>Sign in with Zoom, then rehearse: select a source in Preview, Take it, and make a short test recording before your first show.</li>
</ol>
<p>Pro needs Windows 10 or later, 64-bit, with a Direct3D 11 GPU and current graphics drivers. Installing 1.0 removes earlier CoreVideo Pro betas and keeps your settings and recordings.</p>
<h2>What 1.0 does not do yet</h2>
<p>CoreVideo Pro 1.0 is Windows only. Captions, the automatic scene director, and Blackmagic or AJA capture are not part of this release.</p>
<p>Found a problem? Diagnostics &rarr; Export support bundle collects redacted logs, and <a href="${PRO_REPO}/issues">GitHub issues</a> is where to report it.</p>`;
}

function zcommsPageContent() {
  return `<section class="hero product-hero screenshot-hero">
  <div class="hero-copy">
    <p class="eyebrow">Talkback and IFB station for Windows</p>
    <h1>ZComms</h1>
    <p class="lede">Cue talent privately during a Zoom production. Assign named keys to people or groups and connect feeds from a console or intercom.</p>
    <div class="hero-actions">
      <a class="button primary" href="${ZCOMMS_RELEASES_LATEST}">Download for Windows</a>
      <a class="button" href="#how-it-works">How talkback works</a>
    </div>
    <p class="product-utility"><a href="${ZCOMMS_REPO}">GitHub repository</a></p>
  </div>
  ${productScreenshot(ZCOMMS_PANEL_IMAGE, "ZComms v0.1.14 talkback panel in a sample session")}
</section>
<section class="link-grid" aria-label="What ZComms does">
  <div><strong>A named key per person</strong><span>Every talkback-capable participant gets a standing channel of their own, and the key wears their name. ALL CALL spans the panel; latch makes a press stick.</span></div>
  <div><strong>Private talkback</strong><span>Only the selected guests hear you. Their meeting audio drops while you&rsquo;re keyed and returns to its normal level when you release.</span></div>
  <div><strong>Extern feeds</strong><span>Latch one channel &mdash; or a stereo pair &mdash; of a multichannel interface into a talkback channel, so a larger intercom, a console bus, or a Dante feed uses Zoom as its last mile.</span></div>
  <div><strong>Honest keys</strong><span>A key reads live only while someone is actually hearing you. Keyed into an empty channel reads amber; a person in another breakout room reads dark, names the room, and refuses the press.</span></div>
</section>
<section>
  <h2 id="how-it-works">How a key reaches one ear</h2>
  <p>Zoom&apos;s Meeting SDK carries a talkback path alongside the meeting floor: up to <strong>16 channels</strong>, each holding up to <strong>10 people</strong>, addressed independently of the room. ZComms provisions the whole bank once when it joins and then assigns people to it &mdash; keying selects a channel, it never tears one down and rebuilds it. Your voice goes down the channel you keyed and nowhere else; the meeting mic stays open, because Zoom requires an open mic to deliver talkback at all, but it is fed nothing, so the room stays silent. That is a pure SDK arrangement &mdash; no audio driver is installed and no loopback device is created.</p>
  <figure class="doc-image"><img src="/zcomms/images/zcomms-signal-path.svg" alt="ZComms signal path diagram: the operator microphone runs through input gain, a limiter, a ramped push-to-talk envelope, echo cancellation, and a sidetone tap, and is sent to one Zoom talkback channel that reaches only that channel's members; an extern feed from a multichannel interface can be latched into a channel alongside it, while the meeting mic is held open but fed nothing so the meeting floor hears nothing."></figure>
  <p>Members of the channel you key are <em>ducked</em> rather than muted: their meeting audio drops under your voice and comes back to unity the moment you release, so talent never loses the room. The duck is gated on the signal actually present, not on the state of the key &mdash; a latched extern feed that happens to be silent leaves the room at full level.</p>
</section>
<section>
  <h2>What is on the panel</h2>
  <div class="feature-set">
    <div>
      <h3>Keys and the grid</h3>
      <ul>
        <li>One cell per talkback-capable participant, carrying their Zoom name</li>
        <li>Hold to talk, latch to stick, and LATCH ALL for a standing open line</li>
        <li>ALL CALL across every provisioned channel in one press</li>
        <li>Numbered slots for direct keys, reassignable from the panel</li>
      </ul>
    </div>
    <div>
      <h3>Channels and privacy</h3>
      <ul>
        <li>16 talkback channels, up to 10 listeners each, provisioned in one pass</li>
        <li>Channel isolation verified live &mdash; a listener on one channel hears nothing from another</li>
        <li>Members ducked while you key and returned to unity on release</li>
        <li>The meeting mic held open but fed nothing, and re-asserted as housekeeping</li>
      </ul>
    </div>
    <div>
      <h3>Extern feeds</h3>
      <ul>
        <li>Any channel or stereo pair of a multichannel interface, latched into a channel</li>
        <li>Per-feed gain and limiter, downmixed to mono because SDK sends are mono-only</li>
        <li>Input meter reading &minus;60 to 0 dBFS across 12 segments, pre-envelope and post-gain</li>
        <li>The &minus;50 dBFS signal gate drawn on the scale, so &quot;present&quot; and &quot;counts as signal&quot; are distinguishable</li>
        <li>Feeds persist across launches</li>
      </ul>
    </div>
    <div>
      <h3>Capture chain</h3>
      <ul>
        <li>Input gain, look-ahead limiter, then a ramped PTT envelope &mdash; edges ramp, they do not gate</li>
        <li>Echo cancellation referenced against ZComms&apos; own monitor output, defeatable</li>
        <li>Sidetone tapped after the envelope, so it monitors what is actually being sent</li>
        <li>Device pickers that switch live, plus a built-in test tone</li>
      </ul>
    </div>
    <div>
      <h3>Breakout rooms</h3>
      <ul>
        <li>Zoom talkback cannot cross breakout rooms &mdash; the panel says so rather than failing quietly</li>
        <li>Cells for people in another room go dark, name the room, and refuse the press</li>
        <li>The station can move itself between rooms from Settings</li>
        <li>Chat used as a signalling side-channel for cues and notifications</li>
      </ul>
    </div>
    <div>
      <h3>Joining and control</h3>
      <ul>
        <li>Sign in with Zoom once through the browser (PKCE) &mdash; every join happens as your account</li>
        <li>Paste any meeting link or ID; no bot account and nothing for panelists to install</li>
        <li>The panel is served on <code>127.0.0.1:7350</code> with an SSE state stream</li>
        <li>A one-line action API on the same port &mdash; the seam a Stream Deck or Companion module drives</li>
      </ul>
    </div>
  </div>
</section>
<section>
  <h2>What it needs</h2>
  <ul>
    <li><strong>Windows 10 or Windows 11</strong> (x64). The installer is per-user and needs no administrator rights; the Zoom Meeting SDK ships inside it. There is no macOS build yet.</li>
    <li><strong>A Zoom account you can sign in with.</strong> ZComms joins as you, through the same OAuth broker CoreVideo uses. There are no anonymous joins.</li>
    <li><strong>Panelists on a native Zoom client</strong>, desktop or mobile, with nothing installed on their side. The Zoom <em>web</em> client cannot receive talkback at all &mdash; the panel labels those people rather than pretending the key works.</li>
    <li><strong>A headset on the operator position</strong> is still best practice, echo cancellation notwithstanding.</li>
  </ul>
</section>
<section>
  <h2>Early software, honestly</h2>
  <p>ZComms is at <strong>v${ZCOMMS_VERSION}</strong> and is early. Talkback delivery, channel isolation, the duck behaviour, and breakout awareness have all been verified in real meetings. <strong>Extern feeds have not yet been proven in a live meeting</strong> &mdash; they are covered by unit tests and bench-verified against real hardware, but the in-meeting gates are still open. Expect rough edges, and please report them on the <a href="${ZCOMMS_REPO}/issues">issue tracker</a>.</p>
  <p>The build is <strong>not code-signed</strong>, so Windows SmartScreen warns the first time you run the installer &mdash; &quot;More info&quot;, then &quot;Run anyway&quot;. Signing is waiting on a developer account, not on a decision.</p>
  <div class="hero-actions">
    <a class="button primary" href="${ZCOMMS_RELEASES_LATEST}">Download ZComms for Windows</a>
    <a class="button" href="${ZCOMMS_REPO}">Source &amp; release notes</a>
  </div>
</section>
<section>
  <h2>Where it fits</h2>
  <p>ZComms and CoreVideo are separate products that came out of the same problem &mdash; running a real show inside somebody else&apos;s Zoom meeting &mdash; and they share the Zoom Meeting SDK and the same sign-in broker. ZComms is the standalone desk: it is what you run when the intercom is the job, on its own machine or beside a switcher that has nothing to do with Zoom.</p>
  <p>The <a href="/core-plugin/">CoreVideo plugin</a> includes a Zoom Talkback dock on Windows (v0.1.45 and later), for operators keying talent from OBS. macOS talkback is unavailable. ZComms provides a separate operator position and feeds from a larger comms system.</p>
  <table>
    <thead><tr><th>Product</th><th>Form factor</th><th>Talkback</th></tr></thead>
    <tbody>
      <tr><td><strong>ZComms</strong></td><td>Standalone Windows app</td><td>The whole product &mdash; a full desk, extern feeds, and its own operator position. Shipping now.</td></tr>
      <tr><td><strong>CoreVideo</strong> for OBS</td><td>OBS Studio plugin</td><td>A Zoom Talkback dock beside the video routing, for the operator already cutting the show in OBS. Windows, v0.1.45 and later; unavailable on macOS.</td></tr>
      <tr><td><strong>CoreVideo Pro</strong></td><td>Standalone Windows app</td><td>The Comms page inside the production studio, for the operator running the whole show from Pro. Shipping in <a href="${PRO_ANNOUNCEMENT}">version 1.0</a>.</td></tr>
    </tbody>
  </table>
</section>`;
}

function edgePageContent() {
  return `<section class="hero product-hero">
  <div class="hero-copy">
    <p class="eyebrow">Windows x64 beta</p>
    <h1>CoreVideo Edge</h1>
    <p class="lede">Route and record more Zoom guests on a separate Windows machine. Edge joins the same meeting as <a href="/pro/">CoreVideo Pro</a> under its own Zoom account, giving it a separate downlink budget.</p>
    <div class="hero-actions">
      <a class="button primary" href="${EDGE_DOWNLOADS}/CoreVideoEdge-${EDGE_BETA}-Setup.exe">Download Windows beta</a>
      <a class="button" href="${EDGE_RELEASE}">Release notes &amp; checksums</a>
    </div>
  </div>
  <figure class="hero-media product-shot">
    <img src="/assets/edge-routing.png" width="1280" height="800" alt="Edge Routing screen with Zoom participants assigned to eight slots">
    <figcaption>Participant routing in a sample session</figcaption>
  </figure>
</section>
<section class="link-grid product-features" aria-label="What Edge does">
  <div><strong>Route and monitor guests</strong><span>Choose up to eight Zoom participants and preview each routed feed. Request a resolution and frame rate for each slot. Actual video quality depends on what Zoom delivers.</span></div>
  <div><strong>Record isolated MP4s</strong><span>Arm the guests you want to record. Edge saves one MP4 per armed guest with their own video and isolated audio.</span></div>
  <div><strong>Send feeds to Pro</strong><span>Use NDI or SRT to send routed guests from the Edge machine to your production.</span></div>
</section>
<section>
  <h2>Set up the beta</h2>
  <ol>
    <li>Install Edge on a separate Windows x64 machine. This beta shares Pro&rsquo;s browser sign-in link, so the apps can intercept each other&rsquo;s callbacks if installed together.</li>
    <li>Sign in to Zoom with a separate account and join the same meeting as Pro.</li>
    <li>Choose participants in <strong>Routing</strong>, confirm their previews in <strong>Monitor</strong>, and arm the guests you need for recording.</li>
  </ol>
  <p>NDI output requires the NDI 6 Runtime or NDI Tools. SRT output requires FFmpeg on <code>PATH</code> or at <code>COREVIDEO_FFMPEG_DIR</code>. The beta installer is unsigned. See the release notes for validation details and known limitations.</p>
  <div class="hero-actions">
    <a class="button" href="${EDGE_DOWNLOADS}/CoreVideoEdge-${EDGE_BETA}-win-x64.zip">Portable Windows ZIP</a>
    <a class="button" href="${EDGE_REPO}">View source</a>
  </div>
</section>`;
}

function voltPageContent() {
  return `<section class="hero product-hero">
  <div class="hero-copy">
    <p class="eyebrow">Available from source</p>
    <h1>Volt</h1>
    <p class="lede">Run the show from a shared rundown. Volt brings your media library, clip cues, and publishing together and keeps the same asset ID from ingest through delivery.</p>
    <div class="hero-actions">
      <a class="button primary" href="${VOLT_OPERATIONS}#install">Install from source</a>
      <a class="button" href="${VOLT_REPO}">View source</a>
    </div>
  </div>
  <figure class="hero-media product-shot">
    <img src="/assets/volt-rundown.png" width="1920" height="1080" alt="Volt operator rundown showing the current and next items, the Take button, and armed cues">
    <figcaption>Rundown and cues in a sample session</figcaption>
  </figure>
</section>
<section class="link-grid product-features" aria-label="What Volt does">
  <div><strong>Find the right clip</strong><span>Ingest media and recordings, build proxies, and use transcript search to find the section you need.</span></div>
  <div><strong>Cue the rundown</strong><span>Attach clips and cues to rundown items. With a compatible CoreVideo Pro build, send a cleared clip to Preview and take it to Program when it&rsquo;s ready.</span></div>
  <div><strong>Publish approved clips</strong><span>Create house, streaming, and social versions from the approved clip and track the delivery URLs on its rundown item.</span></div>
</section>
<section>
  <h2>Run Volt</h2>
  <p>The session runs as a service with a browser-based operator interface. A source installation requires Python 3.12 or newer, FFmpeg, and ffprobe. The operations guide covers setup, storage, health checks, and connecting to <a href="/pro/">CoreVideo Pro</a>.</p>
  <a class="button" href="${VOLT_OPERATIONS}">Operations guide</a>
</section>`;
}

function renderTable(lines) {
  const rows = lines
    .filter((line, index) => index !== 1)
    .map((line) =>
      line
        .trim()
        .replace(/^\|/, "")
        .replace(/\|$/, "")
        .split("|")
        .map((cell) => inlineMarkdown(cell.trim())),
    );
  const [head, ...body] = rows;
  return `<table><thead><tr>${head.map((cell) => `<th>${cell}</th>`).join("")}</tr></thead><tbody>${body
    .map((row) => `<tr>${row.map((cell) => `<td>${cell}</td>`).join("")}</tr>`)
    .join("")}</tbody></table>`;
}

function markdownToHtml(markdown) {
  const lines = markdown.replace(/^﻿/, "").replace(/\r\n/g, "\n").split("\n");
  const html = [];
  let list = [];
  let listTag = "ul";
  let paragraph = [];
  let quote = [];
  let table = [];
  let codeBlock = null;

  const flushParagraph = () => {
    if (!paragraph.length) return;
    html.push(`<p>${inlineMarkdown(paragraph.join(" "))}</p>`);
    paragraph = [];
  };
  const flushList = () => {
    if (!list.length) return;
    html.push(`<${listTag}>${list.map((item) => `<li>${inlineMarkdown(item)}</li>`).join("")}</${listTag}>`);
    list = [];
    listTag = "ul";
  };
  const flushQuote = () => {
    if (!quote.length) return;
    html.push(`<blockquote>${quote.map((item) => `<p>${inlineMarkdown(item)}</p>`).join("")}</blockquote>`);
    quote = [];
  };
  const flushTable = () => {
    if (!table.length) return;
    html.push(renderTable(table));
    table = [];
  };
  const flushAll = () => {
    flushParagraph();
    flushList();
    flushQuote();
    flushTable();
  };

  for (const rawLine of lines) {
    const line = rawLine.trimEnd();
    const trimmed = line.trim();

    const fence = /^```([A-Za-z0-9_-]*)\s*$/.exec(trimmed);
    if (fence) {
      if (codeBlock) {
        const languageClass = codeBlock.language ? ` class="language-${escapeHtml(codeBlock.language)}"` : "";
        html.push(`<pre tabindex="0"><code${languageClass}>${escapeHtml(codeBlock.lines.join("\n"))}</code></pre>`);
        codeBlock = null;
      } else {
        flushAll();
        codeBlock = { language: fence[1], lines: [] };
      }
      continue;
    }

    if (codeBlock) {
      codeBlock.lines.push(line);
      continue;
    }

    if (!trimmed) {
      flushAll();
      continue;
    }

    if (trimmed.startsWith("|")) {
      flushParagraph();
      flushList();
      flushQuote();
      table.push(trimmed);
      continue;
    }
    flushTable();

    if (trimmed === "---") {
      flushAll();
      html.push("<hr>");
      continue;
    }

    const heading = /^(#{1,6})\s+(.+)$/.exec(trimmed);
    if (heading) {
      flushAll();
      const level = heading[1].length;
      html.push(`<h${level}>${inlineMarkdown(heading[2])}</h${level}>`);
      continue;
    }

    const bullet = /^[-*]\s+(.+)$/.exec(trimmed);
    if (bullet) {
      flushParagraph();
      flushQuote();
      if (list.length && listTag !== "ul") flushList();
      listTag = "ul";
      list.push(bullet[1]);
      continue;
    }

    const ordered = /^\d+\.\s+(.+)$/.exec(trimmed);
    if (ordered) {
      flushParagraph();
      flushQuote();
      if (list.length && listTag !== "ol") flushList();
      listTag = "ol";
      list.push(ordered[1]);
      continue;
    }

    if (list.length && /^\s+/.test(line)) {
      list[list.length - 1] += ` ${trimmed}`;
      continue;
    }

    if (trimmed.startsWith("> ")) {
      flushParagraph();
      flushList();
      quote.push(trimmed.slice(2));
      continue;
    }

    const image = /^!\[([^\]]*)\]\(([^)]+)\)$/.exec(trimmed);
    if (image) {
      flushAll();
      html.push(renderImage(image[1], image[2]));
      continue;
    }

    paragraph.push(trimmed);
  }
  if (codeBlock) {
    const languageClass = codeBlock.language ? ` class="language-${escapeHtml(codeBlock.language)}"` : "";
    html.push(`<pre><code${languageClass}>${escapeHtml(codeBlock.lines.join("\n"))}</code></pre>`);
  }
  flushAll();
  return html.join("\n");
}

// Open Graph and Twitter cards. Without these, every link posted to Reddit,
// LinkedIn, Slack, or Discord renders as a bare grey URL with no title, blurb,
// or image - which is most of how anyone first sees the project.
function socialTags({
  title, description, url,
  image = OG_IMAGE, imageAlt = OG_IMAGE_ALT, imageWidth = 1200, imageHeight = 630,
}) {
  const tags = [
    ["og:type", "website"],
    ["og:site_name", SITE_NAME],
    ["og:title", title],
    ["og:description", description],
    ["og:image", image],
    ["og:image:alt", imageAlt],
    ["og:image:width", String(imageWidth)],
    ["og:image:height", String(imageHeight)],
    ["og:locale", "en_US"],
  ];
  if (url) tags.push(["og:url", url]);

  const twitter = [
    ["twitter:card", "summary_large_image"],
    ["twitter:title", title],
    ["twitter:description", description],
    ["twitter:image", image],
    ["twitter:image:alt", imageAlt],
  ];

  return [
    ...tags.map(
      ([p, c]) => `  <meta property="${p}" content="${escapeHtml(c)}">`,
    ),
    ...twitter.map(
      ([n, c]) => `  <meta name="${n}" content="${escapeHtml(c)}">`,
    ),
  ].join("\n");
}

// JSON-LD is a data block, not executable script, so the production CSP does
// not apply to it - but the inline-<script> build guard below still has to know
// the difference. Escaping "<" keeps any string in the payload from closing the
// element early.
function jsonLdBlock(data) {
  const json = JSON.stringify(data, null, 2).replaceAll("<", "\\u003c");
  return `  <script type="application/ld+json">\n${json}\n  </script>\n`;
}

function downloadPageContent() {
  const v = RELEASE_VERSION;
  const releaseAssets = v && `https://github.com/iamfatness/CoreVideo/releases/download/v${v}`;
  const windowsInstaller = v ? `${releaseAssets}/CoreVideo-Setup-v${v}.exe` : RELEASES_LATEST;
  const windowsZip = v && `${releaseAssets}/CoreVideo-Windows-x64-v${v}.zip`;

  return `<header class="page-intro">
  <h1>Download CoreVideo for OBS</h1>
  <p class="lede">Choose the installer for the computer running OBS Studio.</p>
  <p><a href="#other-products">Looking for another CoreVideo product?</a></p>
</header>
<div class="download-grid">
  <section class="download-panel" id="windows" aria-labelledby="windows-heading">
    <p class="product-platform">Windows x64${v ? ` / v${v}` : ""}</p>
    <h2 id="windows-heading">Windows</h2>
    <p>Windows 10 or 11 with 64-bit OBS Studio. OBS 32.2.2 or later is recommended.</p>
    <div class="hero-actions">
      <a class="button primary" href="${windowsInstaller}">Download Windows installer</a>
      ${v ? `<a class="button" href="${windowsZip}">Portable ZIP</a>` : ""}
    </div>
    <ul class="download-links">
      <li><a href="${RELEASES_LATEST}">Windows release notes</a></li>
      ${v ? `<li><a href="${windowsInstaller}.sha256">Installer checksum</a></li>
      <li><a href="${windowsZip}.sha256">ZIP checksum</a></li>` : ""}
    </ul>
  </section>
  <section class="download-panel" id="macos" aria-labelledby="macos-heading">
    <p class="product-platform">Apple Silicon / v${MAC_VERSION}</p>
    <h2 id="macos-heading">macOS</h2>
    <p>Apple Silicon Mac with OBS Studio 32.2.1 or later. Developer ID signed and Apple notarized.</p>
    <div class="hero-actions">
      <a class="button primary" href="${MAC_INSTALLER}">Download macOS installer</a>
    </div>
    <ul class="download-links">
      <li><a href="${MAC_RELEASE}">macOS release notes</a></li>
      <li><a href="${MAC_INSTALLER}.sha256">Installer checksum</a></li>
    </ul>
  </section>
</div>
<section>
  <h2>Install the OBS plugin</h2>
  <ol>
    <li>Close OBS Studio.</li>
    <li>Open the installer. On Windows, select your OBS installation folder if it is not detected. On macOS, install for your user account.</li>
    <li>Reopen OBS and follow the <a href="/quickstart/">Operator Quickstart</a> to sign in, join a meeting, and assign your first source.</li>
  </ol>
  <p>The Windows installer is unsigned. Verify the checksum before continuing past SmartScreen&rsquo;s <strong>More info</strong> and <strong>Run anyway</strong> prompts.</p>
  <details class="download-details">
    <summary>Verify an installer</summary>
    <p>Compare the result from PowerShell with the Windows installer checksum:</p>
    <pre><code class="language-powershell">Get-FileHash .\\CoreVideo-Setup-${v ? `v${v}` : "vX.Y.Z"}.exe -Algorithm SHA256</code></pre>
    <p>On macOS, save the installer and checksum in the same folder, then run:</p>
    <pre><code>shasum -a 256 -c CoreVideo-Setup-v${MAC_VERSION}.pkg.sha256</code></pre>
  </details>
  <details class="download-details">
    <summary>Zoom, platform, and recording requirements</summary>
    <p>Sign in with a Zoom account to join meetings. The account&rsquo;s Zoom entitlements and bandwidth determine the number and quality of feeds you can receive. A 1080p feed typically uses 4&ndash;6 Mbps.</p>
    <p>For ISO recording on macOS, choose <strong>Download FFmpeg</strong> in Zoom ISO Recorder or select an existing FFmpeg executable. Talkback is available on Windows. Use OBS&rsquo;s macOS Screen Capture for your own screen and CoreVideo for other participants&rsquo; shares.</p>
    <p>See the <a href="/core-plugin/">Core Plugin Guide</a> for workflows or <a href="/support/">Support</a> for troubleshooting and log collection.</p>
  </details>
</section>
<section>
  <h2>Companion and source builds</h2>
  <p>Download the <a href="https://github.com/iamfatness/CoreVideo/releases/download/v0.1.44/corevideo-obs-1.0.2.tgz">CoreVideo module for Companion v5 or newer</a>. For build instructions, including Linux, see the <a href="https://github.com/iamfatness/CoreVideo">CoreVideo GitHub repository</a>.</p>
</section>
<section id="other-products">
  <h2>Other CoreVideo products</h2>
  <ul class="other-products">
    <li><a href="/pro/">CoreVideo Pro</a><span>Standalone studio for Windows</span></li>
    <li><a href="/edge/">CoreVideo Edge</a><span>Windows beta installer and portable ZIP</span></li>
    <li><a href="/zcomms/">ZComms</a><span>Talkback and IFB for Windows</span></li>
    <li><a href="/volt/">Volt</a><span>Source installation and operations guide</span></li>
  </ul>
</section>`;
}

// Structured data for the free plugin. SoftwareApplication is what earns the
// download-style rich result; the price-0 offer is what marks it free.
function pluginJsonLd() {
  const app = {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    name: "CoreVideo for OBS",
    alternateName: "CoreVideo OBS Plugin",
    applicationCategory: "MultimediaApplication",
    applicationSubCategory: "OBS Studio plugin",
    operatingSystem: "Windows 10, Windows 11, macOS on Apple Silicon",
    description:
      "OBS Studio plugin that captures Zoom meeting participants as native OBS sources - per-participant video and audio, screen share, interpretation audio, and ISO recording - using the Zoom Meeting SDK rather than NDI, a virtual camera, or screen capture.",
    url: `${PRIMARY_ORIGIN}/obs/`,
    downloadUrl: `${PRIMARY_ORIGIN}/download/`,
    installUrl: `${PRIMARY_ORIGIN}/download/`,
    softwareHelp: `${PRIMARY_ORIGIN}/documentation/`,
    license: "https://opensource.org/licenses/MIT",
    isAccessibleForFree: true,
    image: OG_IMAGE,
    offers: {
      "@type": "Offer",
      price: "0",
      priceCurrency: "USD",
      availability: "https://schema.org/InStock",
    },
  };
  if (RELEASE_VERSION) app.softwareVersion = RELEASE_VERSION;

  return app;
}

// Structured data for ZComms. Windows-only and no price claim: the product
// carries no announced pricing, and an invented price-0 offer would be a
// commitment the site is not in a position to make.
function zcommsJsonLd() {
  return {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    name: "ZComms",
    applicationCategory: "MultimediaApplication",
    applicationSubCategory: "Intercom / talkback station",
    operatingSystem: "Windows 10, Windows 11",
    softwareVersion: ZCOMMS_VERSION,
    description:
      "Standalone Windows talkback and IFB station for Zoom productions: named talk keys over the Zoom Meeting SDK's private talkback channels, extern feeds from a multichannel interface, and breakout-aware routing.",
    url: `${PRIMARY_ORIGIN}/zcomms/`,
    downloadUrl: ZCOMMS_RELEASES_LATEST,
    image: screenshotSocialImage(ZCOMMS_PANEL_IMAGE).image,
  };
}

const footerNote =
  "CoreVideo is independent and is not affiliated with Zoom Video Communications, Inc.";

function siteHeader(here) {
  const current = (path) => here === path ? ' aria-current="page"' : "";
  return `<a class="skip-link" href="#main-content">Skip to content</a>
<header class="site-header">
  <div class="site-header-inner">
    <a class="brand" href="/" ${here === "/" ? 'aria-current="page"' : ""}>${MULTIVIEW_MARK}<span>CoreVideo</span></a>
    <nav class="site-nav" aria-label="Main navigation">
      <details class="site-menu">
        <summary>Products<span class="site-chevron" aria-hidden="true"></span></summary>
        <div class="site-menu-panel">
          ${products.map(({name, path}) => `<a href="${path}"${current(path)}>${name}</a>`).join("")}
        </div>
      </details>
      <a href="/documentation/"${current("/documentation/")}>Docs</a>
      <a href="/support/"${current("/support/")}>Support</a>
    </nav>
    <a class="site-download" href="/download/"${current("/download/")}>Downloads</a>
  </div>
</header>`;
}

function siteFooter(note = footerNote) {
  return `<footer class="site-footer">
  <div class="site-footer-inner">
    <a class="brand" href="/">${MULTIVIEW_MARK}<span>CoreVideo</span></a>
    <nav class="footer-links" aria-label="Products">
      ${products.map(({name, path}) => `<a href="${path}">${name}</a>`).join("")}
    </nav>
    <nav class="footer-links" aria-label="Resources">
      <a href="/download/">Downloads</a>
      <a href="/quickstart/">Operator quickstart</a>
      <a href="/documentation/">OBS plugin docs</a>
      <a href="/pro/documentation/">Pro docs</a>
      <a href="/support/">Support</a>
      <a href="https://github.com/iamfatness/CoreVideo">GitHub</a>
    </nav>
    <div class="site-footer-end">
      <span>${note}</span>
      <nav aria-label="Legal"><a href="/privacy/">Privacy</a><a href="/terms/">Terms</a></nav>
    </div>
  </div>
</footer>`;
}

function layout(page, content, options = {}) {
  const here = options.canonical ? options.canonical.replace(PRIMARY_ORIGIN, "") : null;

  // Search results and social cards show the title verbatim, so a page that
  // sets seoTitle owns the whole string; everything else keeps the
  // "<page> | CoreVideo" suffix convention.
  const fullTitle = page.seoTitle ?? `${page.title} | ${SITE_NAME}`;

  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
${GOOGLE_TAG_MANAGER}
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(fullTitle)}</title>
  <meta name="description" content="${escapeHtml(page.description)}">
  <meta name="theme-color" content="#111620">
  <link rel="icon" href="/favicon.svg">
  <link rel="preload" href="/assets/fonts/SpaceGrotesk-var.woff2" as="font" type="font/woff2" crossorigin>
  <link rel="preload" href="/assets/fonts/IBMPlexMono-Regular.woff2" as="font" type="font/woff2" crossorigin>
${options.canonical ? `  <link rel="canonical" href="${escapeHtml(options.canonical)}">\n` : ""}${socialTags({ title: fullTitle, description: page.description, url: options.canonical, ...page.socialImage })}
  <link rel="stylesheet" href="/assets/site-shell.css">
  <link rel="stylesheet" href="/assets/site.css">
  <script defer src="/assets/site-nav.js"></script>
${ANALYTICS_BEACON}${options.jsonLd ? jsonLdBlock(options.jsonLd) : ""}</head>
<body class="${options.home ? "home-page" : "document-page"}">
  ${siteHeader(here)}
  <main class="content" id="main-content" tabindex="-1">
    ${content}
  </main>
  ${siteFooter(options.footerText)}
</body>
</html>
`;
}

// Code blocks and (on phones) tables scroll inside themselves, and a region
// that scrolls must be reachable by keyboard (axe scrollable-region-focusable,
// WCAG 2.1.1). One rule at the single write point instead of at each emitter:
// several emitters already added it to <pre> and several did not.
function focusableScrollRegions(html) {
  return html.replace(/<(pre|table)(?![^>]*\btabindex=)([^>]*)>/g, '<$1 tabindex="0"$2>');
}

function writeText(relativePath, content) {
  const filePath = path.join(outDir, relativePath);
  ensureDir(filePath);
  fs.writeFileSync(filePath, relativePath.endsWith(".html") ? focusableScrollRegions(content) : content, "utf8");
}

// Clean the output directory. On CI (fresh checkout) this always succeeds. On a
// dev machine a process serving public/ (e.g. a local http server) can hold a
// handle that makes removal fail; in that case warn loudly and overwrite in
// place — every file below is rewritten, so output stays correct.
try {
  fs.rmSync(outDir, { recursive: true, force: true });
} catch (err) {
  console.warn(
    `Warning: could not remove ${path.relative(root, outDir)} (${err.code}); ` +
      `overwriting in place. Close any process serving public/ if stale files linger.`,
  );
}

for (const page of pages) {
  const markdown = normalizeText(
    fs.readFileSync(path.join(wikiDir, page.source), "utf8"),
  );
  const isHome = page.output === "index.html";
  const html = layout(page, isHome ? homeContent() : markdownToHtml(markdown), {
    home: isHome,
    canonical: canonicalUrl(page.output),
    jsonLd: isHome ? {
      "@context": "https://schema.org",
      "@type": "WebSite",
      name: SITE_NAME,
      url: `${PRIMARY_ORIGIN}/`,
    } : undefined,
  });
  writeText(page.output, html);
  for (const alias of page.aliases ?? []) {
    writeText(alias, html);
  }
}

for (const page of markdownPages) {
  const markdown = normalizeText(fs.readFileSync(page.source, "utf8"));
  const html = layout(page, markdownToHtml(markdown), {
    canonical: canonicalUrl(page.output),
  });
  writeText(page.output, html);
}

// Download page. This used to be a bare 302 from the worker straight to the
// GitHub releases list, which meant the highest-intent page on the site was not
// a page at all - nothing to rank, and nothing to read before installing.
writeText(
  "obs/index.html",
  layout(
    {
      title: "CoreVideo for OBS",
      seoTitle: "CoreVideo for OBS - Native Zoom Video and Audio Sources",
      description: "Bring individual Zoom guests, audio, screen shares, and interpretation channels into OBS Studio. Free for Windows and Apple Silicon.",
    },
    obsPageContent(),
    { home: true, canonical: canonicalUrl("obs/index.html"), jsonLd: pluginJsonLd() },
  ),
);

writeText(
  "download/index.html",
  layout(
    {
      title: "Download",
      seoTitle: "Download CoreVideo - Free Zoom Plugin for OBS Studio",
      description: RELEASE_VERSION
        ? `Download CoreVideo v${RELEASE_VERSION}, the free open-source OBS Studio plugin for Zoom. Windows and macOS installers, checksums, and setup.`
        : "Download CoreVideo, the free open-source OBS Studio plugin for Zoom. Windows and macOS installers, checksums, and setup.",
    },
    downloadPageContent(),
    {
      canonical: canonicalUrl("download/index.html"),
      jsonLd: pluginJsonLd(),
    },
  ),
);

// CoreVideo Pro landing page
writeText(
  "pro/index.html",
  layout(
    {
      title: "CoreVideo Pro",
      seoTitle: "CoreVideo Pro 1.0 - Zoom Production Studio for Windows",
      description:
        "CoreVideo Pro 1.0 for Windows: Zoom guests as 1080p sources, scenes with Take, in-meeting talkback, ISO recording, streaming, NDI and a virtual camera.",
      socialImage: screenshotSocialImage(PRO_STUDIO_IMAGE),
    },
    proPageContent(),
    {
      home: true,
      canonical: canonicalUrl("pro/index.html"),
      footerText:
        "CoreVideo and CoreVideo Pro are independent products and are not affiliated with Zoom Video Communications, Inc.",
    },
  ),
);

// CoreVideo Pro documentation page
writeText(
  "pro/documentation/index.html",
  layout(
    {
      title: "CoreVideo Pro Architecture",
      description:
        "How CoreVideo Pro 1.0 is built: WinUI console, C++ media core, isolated Zoom engine, Direct3D 11 compositor, and recording, ISO, stream, NDI and camera outputs.",
    },
    proDocsContent(),
    {
      canonical: canonicalUrl("pro/documentation/index.html"),
      footerText:
        "CoreVideo and CoreVideo Pro are independent products and are not affiliated with Zoom Video Communications, Inc.",
    },
  ),
);

// CoreVideo Pro 1.0 announcement (linked from the home page strip).
writeText(
  "news/corevideo-pro-1-0/index.html",
  layout(
    {
      title: "CoreVideo Pro 1.0 is here",
      seoTitle: "CoreVideo Pro 1.0 Released - Zoom Production Studio for Windows",
      description:
        "CoreVideo Pro 1.0, the first stable release of the standalone Zoom production studio for Windows: scenes, in-meeting talkback, ISO recording and every output.",
    },
    proAnnouncementContent(),
    {
      canonical: canonicalUrl("news/corevideo-pro-1-0/index.html"),
      footerText:
        "CoreVideo and CoreVideo Pro are independent products and are not affiliated with Zoom Video Communications, Inc.",
    },
  ),
);

writeText(
  "edge/index.html",
  layout(
    {
      title: "CoreVideo Edge",
      seoTitle: "CoreVideo Edge - Zoom Routing and ISO Recording",
      description:
        "Route, preview, and record additional Zoom participants on a separate Windows machine, then send their feeds to CoreVideo Pro over NDI or SRT.",
      socialImage: {
        image: `${PRIMARY_ORIGIN}/assets/edge-routing.png`,
        imageAlt: "CoreVideo Edge participant routing screen",
        imageWidth: 1280,
        imageHeight: 800,
      },
    },
    edgePageContent(),
    { home: true, canonical: canonicalUrl("edge/index.html") },
  ),
);

writeText(
  "volt/index.html",
  layout(
    {
      title: "Volt",
      seoTitle: "CoreVideo Volt - Show Rundown, Media Library, and Publishing",
      description:
        "Run a show from a shared rundown, find and cue clips, and publish approved versions from one CoreVideo Volt session.",
      socialImage: {
        image: `${PRIMARY_ORIGIN}/assets/volt-rundown.png`,
        imageAlt: "Volt operator rundown and cue controls",
        imageWidth: 1920,
        imageHeight: 1080,
      },
    },
    voltPageContent(),
    { home: true, canonical: canonicalUrl("volt/index.html") },
  ),
);

// ZComms has its own repository and release cadence.
writeText(
  "zcomms/index.html",
  layout(
    {
      title: "ZComms",
      seoTitle: "ZComms - Zoom Talkback & Intercom Station for Live Production",
      description:
        "Standalone Windows talkback and IFB station for Zoom: named talk keys, 16 private channels, extern feeds from a multichannel interface, breakout-aware.",
      socialImage: screenshotSocialImage(ZCOMMS_PANEL_IMAGE),
    },
    zcommsPageContent(),
    {
      home: true,
      canonical: canonicalUrl("zcomms/index.html"),
      jsonLd: zcommsJsonLd(),
      footerText:
        "ZComms is an independent product and is not affiliated with Zoom Video Communications, Inc.",
    },
  ),
);

// ZComms talkback signal path. Drawn in the site's own tokens (unlike the two
// older Pro diagrams, which predate the console design language).
writeText(
  "zcomms/images/zcomms-signal-path.svg",
  `<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="470" viewBox="0 0 1120 470" role="img" aria-labelledby="zcsp-t zcsp-d">
  <title id="zcsp-t">ZComms talkback signal path</title>
  <desc id="zcsp-d">The operator microphone runs through input gain, a limiter, a ramped push-to-talk envelope, echo cancellation, and a sidetone tap, then is sent to one Zoom talkback channel that reaches only that channel's members. An extern feed - one channel or a stereo pair of a multichannel interface, downmixed to mono - can be latched into a channel alongside it. The meeting microphone is held open, because Zoom requires an open mic to deliver talkback, but is fed nothing, so the meeting floor hears nothing.</desc>
  <defs>
    <style>
      .bg{fill:#0a0b0c}
      .card{fill:#101315;stroke:rgba(255,255,255,.16);stroke-width:1.2}
      .live{fill:#101315;stroke:#22c86e;stroke-width:1.6}
      .field{fill:#0e1112;stroke:rgba(255,255,255,.12);stroke-width:1.2}
      .t{font-family:'Space Grotesk',Segoe UI,Arial,sans-serif;fill:#e9edef;font-size:15px;font-weight:600}
      .m{font-family:'IBM Plex Mono',Consolas,monospace;fill:#8b949b;font-size:12px}
      .lbl{font-family:'IBM Plex Mono',Consolas,monospace;fill:#22c86e;font-size:11px;letter-spacing:.08em}
      .dim{font-family:'IBM Plex Mono',Consolas,monospace;fill:#8b949b;font-size:11px;letter-spacing:.08em}
      .flow{stroke:#22c86e;stroke-width:2;fill:none;marker-end:url(#zcsp-a)}
      .flow-dim{stroke:#5c656b;stroke-width:2;fill:none;marker-end:url(#zcsp-b);stroke-dasharray:5 4}
    </style>
    <marker id="zcsp-a" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto" markerUnits="strokeWidth"><path d="M0,0 L0,6 L8,3 z" fill="#22c86e"/></marker>
    <marker id="zcsp-b" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto" markerUnits="strokeWidth"><path d="M0,0 L0,6 L8,3 z" fill="#5c656b"/></marker>
  </defs>
  <rect class="bg" width="1120" height="470"/>

  <rect class="card" x="28" y="52" width="176" height="96" rx="10"/>
  <text class="t" x="48" y="86">Your mic</text>
  <text class="m" x="48" y="110">operator position,</text>
  <text class="m" x="48" y="128">headset recommended</text>

  <rect class="card" x="252" y="34" width="252" height="132" rx="10"/>
  <text class="lbl" x="272" y="60">CAPTURE CHAIN</text>
  <text class="m" x="272" y="84">input gain</text>
  <text class="m" x="272" y="104">look-ahead limiter</text>
  <text class="m" x="272" y="124">ramped PTT envelope</text>
  <text class="m" x="272" y="144">echo cancellation &#xB7; sidetone tap</text>

  <rect class="live" x="552" y="34" width="216" height="132" rx="10"/>
  <text class="lbl" x="572" y="60">TALKBACK CHANNEL</text>
  <text class="t" x="572" y="88">One of 16</text>
  <text class="m" x="572" y="112">up to 10 listeners</text>
  <text class="m" x="572" y="132">addressed on its own path,</text>
  <text class="m" x="572" y="150">not through the room</text>

  <rect class="live" x="816" y="52" width="276" height="96" rx="10"/>
  <text class="t" x="836" y="86">The panelist&#x2019;s ear</text>
  <text class="m" x="836" y="110">stock Zoom desktop or mobile,</text>
  <text class="m" x="836" y="128">nothing installed on their side</text>

  <path class="flow" d="M204 100 L246 100"/>
  <path class="flow" d="M504 100 L546 100"/>
  <path class="flow" d="M768 100 L810 100"/>

  <rect class="field" x="252" y="216" width="252" height="112" rx="10"/>
  <text class="dim" x="272" y="242">EXTERN FEED</text>
  <text class="m" x="272" y="266">one channel or a stereo pair</text>
  <text class="m" x="272" y="286">of a multichannel interface,</text>
  <text class="m" x="272" y="306">downmixed to mono, latched</text>
  <path class="flow" d="M504 260 C540 260 546 200 558 172"/>

  <rect class="field" x="28" y="366" width="740" height="76" rx="10"/>
  <text class="dim" x="48" y="392">MEETING MIC</text>
  <text class="m" x="48" y="416">held open, because Zoom delivers talkback only from an open mic &#x2014; and fed nothing,</text>
  <text class="m" x="48" y="434">through the SDK&#x2019;s own external audio source. No driver, no loopback device.</text>
  <rect class="field" x="816" y="366" width="276" height="76" rx="10"/>
  <text class="dim" x="836" y="392">THE MEETING FLOOR</text>
  <text class="m" x="836" y="416">hears nothing you key.</text>
  <text class="m" x="836" y="434">Members duck, they do not mute.</text>
  <path class="flow-dim" d="M768 404 L810 404"/>
</svg>`,
);

// CoreVideo Pro system architecture diagram
writeText(
  "pro/images/corevideo-pro-architecture.svg",
  `<svg xmlns="http://www.w3.org/2000/svg" width="1160" height="640" viewBox="0 0 1160 640" role="img" aria-labelledby="cvpa-t cvpa-d">
  <title id="cvpa-t">CoreVideo Pro system architecture</title>
  <desc id="cvpa-d">The React/Vite operator renderer drives a native C++ media core over typed IPC. An isolated Zoom Meeting SDK process and local Blackmagic/AJA capture feed raw frames into the core, which composites on the GPU and produces program recording, per-guest ISO, and RTMP/NDI/SRT streaming outputs.</desc>
  <defs>
    <style>
      .bg{fill:#07101c}.panel{fill:#111827;stroke:#2dd4bf;stroke-width:2}.sub{fill:#0b1220;stroke:#334155;stroke-width:1.5}.text{font-family:Segoe UI,Arial,sans-serif;fill:#f8fafc}.muted{font-family:Segoe UI,Arial,sans-serif;fill:#94a3b8}.accent{font-family:Segoe UI,Arial,sans-serif;fill:#67e8f9}.arrow{stroke:#67e8f9;stroke-width:3;fill:none;marker-end:url(#cvpa-arr)}
    </style>
    <marker id="cvpa-arr" markerWidth="10" markerHeight="10" refX="9" refY="3" orient="auto" markerUnits="strokeWidth"><path d="M0,0 L0,6 L9,3 z" fill="#67e8f9"/></marker>
  </defs>
  <rect class="bg" width="1160" height="640"/>
  <rect class="panel" x="40" y="40" width="1080" height="124" rx="14"/>
  <text class="text" x="70" y="78" font-size="24" font-weight="700">Operator Renderer &#x2014; React / Vite</text>
  <text class="muted" x="70" y="102" font-size="14">renders the UI only &#x2014; never owns media; swappable Electron / Tauri / native shell</text>
  <rect class="sub" x="70" y="114" width="232" height="36" rx="8"/><text class="text" x="86" y="138" font-size="14">Production Console</text>
  <rect class="sub" x="318" y="114" width="232" height="36" rx="8"/><text class="text" x="334" y="138" font-size="14">Scenes &amp; Templates</text>
  <rect class="sub" x="566" y="114" width="256" height="36" rx="8"/><text class="text" x="582" y="138" font-size="14">Source &amp; Audio Panels</text>
  <rect class="sub" x="838" y="114" width="252" height="36" rx="8"/><text class="text" x="854" y="138" font-size="14">Keyboard Command Layer</text>
  <path class="arrow" d="M580 164 L580 214"/>
  <text class="accent" x="592" y="194" font-size="14">typed IPC &#xB7; commands + state</text>
  <rect class="panel" x="40" y="224" width="300" height="384" rx="14"/>
  <text class="text" x="68" y="262" font-size="20" font-weight="700">Capture</text>
  <rect class="sub" x="68" y="282" width="244" height="118" rx="10"/>
  <text class="text" x="86" y="312" font-size="16">Zoom Meeting SDK</text>
  <text class="muted" x="86" y="336" font-size="13">isolated process &#xB7; raw video,</text>
  <text class="muted" x="86" y="356" font-size="13">audio, and screen share</text>
  <text class="muted" x="86" y="382" font-size="13">auto-reconnect &#xB7; webinar mode</text>
  <rect class="sub" x="68" y="416" width="244" height="106" rx="10"/>
  <text class="text" x="86" y="446" font-size="16">Local Capture</text>
  <text class="muted" x="86" y="470" font-size="13">Blackmagic DeckLink /</text>
  <text class="muted" x="86" y="490" font-size="13">UltraStudio &#xB7; AJA Io / Kona</text>
  <rect class="panel" x="420" y="224" width="320" height="384" rx="14"/>
  <text class="text" x="448" y="262" font-size="20" font-weight="700">Native Media Core &#x2014; C++</text>
  <rect class="sub" x="448" y="282" width="264" height="40" rx="8"/><text class="text" x="464" y="308" font-size="15">GPU Compositor &#xB7; D3D 11/12 &#xB7; Metal</text>
  <rect class="sub" x="448" y="330" width="264" height="40" rx="8"/><text class="text" x="464" y="356" font-size="15">Scene Graph Engine</text>
  <rect class="sub" x="448" y="378" width="264" height="40" rx="8"/><text class="text" x="464" y="404" font-size="15">Smart Audio Mixer</text>
  <rect class="sub" x="448" y="426" width="264" height="40" rx="8"/><text class="text" x="464" y="452" font-size="15">Graphics &#xB7; Lower-thirds &#xB7; Captions</text>
  <rect class="sub" x="448" y="474" width="264" height="48" rx="8"/><text class="text" x="464" y="498" font-size="15">AI Director</text><text class="muted" x="464" y="516" font-size="12">Magic Scene &#xB7; Set &amp; Forget</text>
  <rect class="sub" x="448" y="530" width="264" height="40" rx="8"/><text class="text" x="464" y="556" font-size="15">Hardware Encoder</text>
  <rect class="panel" x="820" y="224" width="300" height="384" rx="14"/>
  <text class="text" x="848" y="262" font-size="20" font-weight="700">Outputs</text>
  <rect class="sub" x="848" y="282" width="244" height="46" rx="8"/><text class="text" x="866" y="310" font-size="15">Program Record &#xB7; MP4 / MOV</text>
  <rect class="sub" x="848" y="338" width="244" height="46" rx="8"/><text class="text" x="866" y="366" font-size="15">Per-guest ISO Record</text>
  <rect class="sub" x="848" y="394" width="244" height="46" rx="8"/><text class="text" x="866" y="422" font-size="15">RTMP / NDI / SRT Stream</text>
  <rect class="sub" x="848" y="450" width="244" height="46" rx="8"/><text class="text" x="866" y="478" font-size="15">WebRTC Monitor</text>
  <path class="arrow" d="M340 414 L420 414"/>
  <text class="accent" x="344" y="402" font-size="12">raw frames &#xB7; media bus</text>
  <path class="arrow" d="M740 414 L820 414"/>
  <text class="accent" x="744" y="402" font-size="12">encoded program + ISO</text>
</svg>`,
);

// CoreVideo Pro production pipeline diagram
writeText(
  "pro/images/corevideo-pro-pipeline.svg",
  `<svg xmlns="http://www.w3.org/2000/svg" width="1160" height="460" viewBox="0 0 1160 460" role="img" aria-labelledby="cvpp-t cvpp-d">
  <title id="cvpp-t">CoreVideo Pro production pipeline</title>
  <desc id="cvpp-d">Zoom participants and local cameras are captured and synchronised, composited by the GPU scene-graph engine under direction from the AI director and audio mixer, and produced as a program feed that is encoded to recording, ISO, and streaming outputs.</desc>
  <defs>
    <style>
      .bg{fill:#07101c}.stage{fill:#111827;stroke:#2dd4bf;stroke-width:2}.aux{fill:#0b1220;stroke:#334155;stroke-width:1.5}.text{font-family:Segoe UI,Arial,sans-serif;fill:#f8fafc}.muted{font-family:Segoe UI,Arial,sans-serif;fill:#94a3b8}.accent{font-family:Segoe UI,Arial,sans-serif;fill:#67e8f9}.flow{stroke:#67e8f9;stroke-width:3;fill:none;marker-end:url(#cvpp-arr)}
    </style>
    <marker id="cvpp-arr" markerWidth="10" markerHeight="10" refX="9" refY="3" orient="auto" markerUnits="strokeWidth"><path d="M0,0 L0,6 L9,3 z" fill="#67e8f9"/></marker>
  </defs>
  <rect class="bg" width="1160" height="460"/>
  <rect class="stage" x="30" y="180" width="180" height="100" rx="12"/>
  <text class="text" x="50" y="214" font-size="16" font-weight="700">Sources</text>
  <text class="muted" x="50" y="240" font-size="13">Zoom participants</text>
  <text class="muted" x="50" y="260" font-size="13">+ local cameras</text>
  <rect class="stage" x="270" y="180" width="180" height="100" rx="12"/>
  <text class="text" x="290" y="214" font-size="16" font-weight="700">Capture &amp; Sync</text>
  <text class="muted" x="290" y="240" font-size="13">SDK + SDI/HDMI</text>
  <text class="muted" x="290" y="260" font-size="13">A/V alignment</text>
  <rect class="stage" x="510" y="160" width="190" height="140" rx="12"/>
  <text class="text" x="530" y="194" font-size="16" font-weight="700">Scene Graph</text>
  <text class="text" x="530" y="214" font-size="16" font-weight="700">Compositor</text>
  <text class="muted" x="530" y="240" font-size="13">GPU &#xB7; slots,</text>
  <text class="muted" x="530" y="260" font-size="13">transitions,</text>
  <text class="muted" x="530" y="280" font-size="13">graphics &amp; captions</text>
  <rect class="stage" x="760" y="180" width="160" height="100" rx="12"/>
  <text class="text" x="780" y="214" font-size="16" font-weight="700">Program</text>
  <text class="muted" x="780" y="240" font-size="13">Take &#xB7; Cut /</text>
  <text class="muted" x="780" y="260" font-size="13">Fade / Slide</text>
  <rect class="stage" x="980" y="180" width="150" height="100" rx="12"/>
  <text class="text" x="1000" y="214" font-size="16" font-weight="700">Encode</text>
  <text class="muted" x="1000" y="240" font-size="13">Record &#xB7; ISO</text>
  <text class="muted" x="1000" y="260" font-size="13">RTMP/NDI/SRT</text>
  <rect class="aux" x="510" y="40" width="190" height="70" rx="10"/>
  <text class="text" x="530" y="70" font-size="15" font-weight="700">AI Director</text>
  <text class="muted" x="530" y="92" font-size="12">Magic Scene &#xB7; Set &amp; Forget</text>
  <rect class="aux" x="270" y="350" width="430" height="70" rx="10"/>
  <text class="text" x="290" y="380" font-size="15" font-weight="700">Smart Audio Mixer</text>
  <text class="muted" x="290" y="402" font-size="12">per-source gain, leveling, limiter &#x2192; mixed program bus</text>
  <path class="flow" d="M210 230 L270 230"/>
  <path class="flow" d="M450 230 L510 230"/>
  <path class="flow" d="M700 230 L760 230"/>
  <path class="flow" d="M920 230 L980 230"/>
  <path class="flow" d="M605 110 L605 160"/>
  <text class="accent" x="616" y="140" font-size="12">scene decisions</text>
  <path class="flow" d="M700 360 C760 360 800 300 810 282"/>
  <text class="accent" x="708" y="338" font-size="12">mixed audio</text>
</svg>`,
);

// Shared site imagery used in page heroes and social cards.
for (const name of [
  "corevideo-share.jpg",
  "obs-multiview.webp",
  "dynamic-gallery.webp",
  "edge-routing.png",
  "volt-rundown.png",
  PRO_STUDIO_IMAGE.file,
  ZCOMMS_PANEL_IMAGE.file,
]) {
  const source = path.join(siteAssetsDir, name);
  if (fs.existsSync(source)) {
    const target = path.join(outDir, "assets", name);
    ensureDir(target);
    fs.copyFileSync(source, target);
  } else {
    console.warn(`Warning: ${name} missing from site-assets; skipping.`);
  }
}

const logoSource = path.join(siteAssetsDir, "corevideo-logo.jpg");
if (fs.existsSync(logoSource)) {
  const logoTarget = path.join(outDir, "assets", "corevideo-logo.jpg");
  ensureDir(logoTarget);
  fs.copyFileSync(logoSource, logoTarget);
}

const proStudioSource = path.join(siteAssetsDir, "corevideo-pro-studio.webp");
// Owner review pages have their own URLs, but are not navigation or sitemap entries.
const reviewPagesSource = path.join(siteAssetsDir, "reviews");
if (fs.existsSync(reviewPagesSource)) {
  fs.cpSync(reviewPagesSource, path.join(outDir, "reviews"), { recursive: true });
}
if (fs.existsSync(proStudioSource)) {
  const proStudioTarget = path.join(outDir, "pro", "images", "corevideo-pro-studio.webp");
  ensureDir(proStudioTarget);
  fs.copyFileSync(proStudioSource, proStudioTarget);
}

const docsImagesSource = path.join(docsDir, "images");
if (fs.existsSync(docsImagesSource)) {
  fs.cpSync(docsImagesSource, path.join(outDir, "core-plugin", "images"), {
    recursive: true,
  });
}

// Plugin Docs screenshots (served at /documentation/images/).
const pluginDocShots = path.join(siteAssetsDir, "plugin-docs");
if (fs.existsSync(pluginDocShots)) {
  fs.cpSync(pluginDocShots, path.join(outDir, "documentation", "images"), {
    recursive: true,
  });
}

// Mirrors the <title> already inside docs/index.html.
const DOCS_TITLE = "CoreVideo - OBS Plugin for Live Zoom Video";
const DOCS_DESCRIPTION =
  "How the CoreVideo OBS plugin works: the Zoom Meeting SDK engine process, IPC and shared-memory frame transport, source types, active-speaker direction, and setup.";
const DOCS_CANONICAL = canonicalUrl("documentation/index.html");

const docsHtml = fs.readFileSync(path.join(docsDir, "index.html"), "utf8")
  .replace("<!-- site-header -->", siteHeader("/documentation/"))
  .replace("<!-- site-footer -->", siteFooter())
  .replace(/(<meta charset="UTF-8"\s*\/>)/i, `$1\n${GOOGLE_TAG_MANAGER}`)
  .replaceAll("iamfatness.github.io/CoreVideo", publicDocumentationUrl
    ? new URL("/documentation", publicDocumentationUrl).host + "/documentation"
    : "CoreVideo documentation")
  .replaceAll("https://iamfatness.github.io/CoreVideo/", "/documentation/")
  .replaceAll('href="ZOOM_MARKETPLACE_OAUTH.md"', 'href="https://github.com/iamfatness/CoreVideo/blob/main/docs/ZOOM_MARKETPLACE_OAUTH.md"')
  .replaceAll('href="ROADMAP.md"', 'href="https://github.com/iamfatness/CoreVideo/blob/main/docs/ROADMAP.md"')
  .replaceAll("<pre>", '<pre tabindex="0">')
  // The documentation page ships its own hand-written <head>, so it misses the
  // shared layout(). Inject the same canonical, description, and social cards
  // here rather than leaving the most-linked page on the site without them.
  .replace(
    "</head>",
    `  <link rel="canonical" href="${DOCS_CANONICAL}">\n` +
      `  <meta name="description" content="${escapeHtml(DOCS_DESCRIPTION)}">\n` +
      socialTags({
        title: DOCS_TITLE,
        description: DOCS_DESCRIPTION,
        url: DOCS_CANONICAL,
        image: OG_IMAGE,
      }) +
      "\n" + ANALYTICS_BEACON + "</head>",
  );
if (!docsHtml.includes('rel="canonical"')) {
  throw new Error(
    "docs/index.html has no </head> to inject SEO tags into; the documentation " +
      "page would ship without a canonical, description, or social card.",
  );
}
writeText("documentation/index.html", docsHtml);
writeText("docs/index.html", docsHtml);

for (const asset of ["site-shell.css", "site.css", "site-nav.js"]) {
  writeText(`assets/${asset}`, fs.readFileSync(path.join(siteAssetsDir, asset), "utf8"));
}

// Docs-page script (mermaid theme init + sidebar highlighting). Must be an
// external self-hosted file: the worker's CSP has script-src 'self' + jsdelivr
// with no 'unsafe-inline', so inline scripts are silently blocked in
// production (that is how the mermaid theme config never applied for months).
writeText(
  "assets/docs-init.js",
  fs.readFileSync(path.join(siteAssetsDir, "docs-init.js"), "utf8"),
);

writeText(
  "assets/google-tag-manager.js",
  fs.readFileSync(path.join(siteAssetsDir, "google-tag-manager.js"), "utf8"),
);

// Bundled application fonts (Space Grotesk + IBM Plex Mono), self-hosted so
// the CSP font-src 'self' policy allows them.
const fontsSource = path.join(siteAssetsDir, "fonts");
if (fs.existsSync(fontsSource)) {
  fs.cpSync(fontsSource, path.join(outDir, "assets", "fonts"), { recursive: true });
}

// Favicon: the Multiview brand mark on the app background.
writeText(
  "favicon.svg",
  `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" fill="#0A0B0C"/><rect x="6" y="8" width="20" height="16" rx="3" fill="none" stroke="#E9EDEF" stroke-width="1.6"/><line x1="16" y1="9" x2="16" y2="23" stroke="#E9EDEF" stroke-width="1.1" opacity=".5"/><line x1="7" y1="16" x2="25" y2="16" stroke="#E9EDEF" stroke-width="1.1" opacity=".5"/><rect x="17" y="9.2" width="8.4" height="6" rx="1" fill="#22C86E"/></svg>
`,
);

writeText("_redirects", `/terms-of-use /terms/ 301
/Terms-of-Use /terms/ 301
/privacy-policy /privacy/ 301
/Privacy-Policy /privacy/ 301
/Support /support/ 301
/docs /documentation/ 301
/oauth /documentation/#flow-oauth 301
/oauth/ /documentation/#flow-oauth 301
`);
writeText("CNAME", "corevideo.io\n");

// Sitemap: every canonical URL registered during the build, so a new page is
// listed the moment it is generated rather than when someone remembers to
// update a hand-maintained list.
const sitemapUrls = [...indexableUrls].sort();
writeText(
  "sitemap.xml",
  `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
${sitemapUrls.map((url) => `  <url><loc>${escapeHtml(url)}</loc></url>`).join("\n")}
</urlset>
`,
);

// Cloudflare serves a managed robots.txt when the origin has none, and that one
// carries no Sitemap: line. Shipping our own means the sitemap is advertised.
writeText(
  "robots.txt",
  `User-agent: *
Allow: /

Sitemap: ${PRIMARY_ORIGIN}/sitemap.xml
`,
);

// Guard: the production CSP blocks inline scripts, so any inline <script> in
// built HTML is a page that will silently misbehave in production only.
// Fail the build loudly instead.
function findHtmlFiles(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? findHtmlFiles(p) : e.name.endsWith(".html") ? [p] : [];
  });
}
// application/ld+json is structured-data markup that browsers never execute, so
// script-src does not apply to it and it is exempt from this guard.
const inlineOffenders = findHtmlFiles(outDir).filter((f) =>
  /<script(?![^>]*\bsrc=)(?![^>]*type="application\/ld\+json")[^>]*>/i.test(
    fs.readFileSync(f, "utf8"),
  ),
);
if (inlineOffenders.length) {
  const list = inlineOffenders.map((f) => path.relative(outDir, f)).join(", ");
  throw new Error(
    `Inline <script> blocked by the production CSP found in: ${list}. ` +
      "Move the code into an external file under assets/ instead.",
  );
}

console.log(`Built CoreVideo site in ${path.relative(root, outDir)}`);
