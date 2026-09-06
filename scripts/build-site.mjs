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
// macOS betas are signed locally and published separately from Windows releases.
const MAC_VERSION = "0.1.45-beta.1";
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

// The CoreVideo "Multiview" brand mark (mirrors Controls/MultiviewMark.xaml in
// the app): a 16:9 monitor frame split 2x2 with the top-right tile live-green.
const MULTIVIEW_MARK =
  `<svg class="mv" viewBox="0 0 32 24" width="28" height="21" fill="none" aria-hidden="true">` +
  `<rect x="1" y="1" width="30" height="22" rx="4" stroke="currentColor" stroke-width="1.6"/>` +
  `<line x1="16" y1="2" x2="16" y2="22" stroke="currentColor" stroke-width="1.2" opacity=".45"/>` +
  `<line x1="2" y1="12" x2="30" y2="12" stroke="currentColor" stroke-width="1.2" opacity=".45"/>` +
  `<rect x="17.4" y="2.6" width="12.8" height="8.6" rx="1.6" fill="#22c86e"/></svg>`;

// The ZComms mark: an intercom panel of six talk keys with one key lit, the
// same idiom as the Multiview mark (a grid with one live cell) applied to a
// hardware talkback desk rather than a monitor wall.
const TALKBACK_MARK =
  `<svg class="mv" viewBox="0 0 32 24" width="28" height="21" fill="none" aria-hidden="true">` +
  `<rect x="1" y="1" width="30" height="22" rx="4" stroke="currentColor" stroke-width="1.6"/>` +
  `<rect x="4.6" y="4.6" width="7" height="5.6" rx="1.4" fill="currentColor" opacity=".38"/>` +
  `<rect x="12.5" y="4.6" width="7" height="5.6" rx="1.4" fill="currentColor" opacity=".38"/>` +
  `<rect x="20.4" y="4.6" width="7" height="5.6" rx="1.4" fill="#22c86e"/>` +
  `<rect x="4.6" y="13.8" width="7" height="5.6" rx="1.4" fill="currentColor" opacity=".38"/>` +
  `<rect x="12.5" y="13.8" width="7" height="5.6" rx="1.4" fill="currentColor" opacity=".38"/>` +
  `<rect x="20.4" y="13.8" width="7" height="5.6" rx="1.4" fill="currentColor" opacity=".38"/></svg>`;

const pages = [
  {
    source: "Home.md",
    title: "CoreVideo",
    seoTitle: "CoreVideo - Zoom Video & Audio as Native OBS Sources",
    description:
      "Free open-source OBS Studio plugin: Zoom participant video, audio, and screen share as native OBS sources. No NDI, virtual camera, or screen capture.",
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

function homeContent() {
  return `<section class="hero">
  <figure class="hero-media">
    <div class="console">
      <div class="console-bar"><span class="tally tally-live">Live</span><span>Program</span><span class="tc">1080p60 &middot; 6 Zoom sources</span></div>
      <div class="console-screen"><img src="/assets/dynamic-gallery.webp" alt="A six-participant gallery composed in OBS from individual CoreVideo sources, each guest in their own rounded, bordered tile on a branded background"></div>
    </div>
  </figure>
  <div class="hero-copy">
    <p class="eyebrow">Free, open-source OBS Studio plugin</p>
    <h1>Bring Zoom participants into OBS as native sources.</h1>
    <p class="lede">CoreVideo joins the meeting through the Zoom Meeting SDK and hands OBS each participant's own video and audio as a real source &mdash; no NDI, no virtual camera, no screen capture, and no second machine. Follow the active speaker, pin spotlight slots, take the screen share, and record every guest to an isolated file.</p>
    <div class="hero-actions">
      <a class="button primary" href="/download/">Download${RELEASE_VERSION ? ` v${RELEASE_VERSION}` : ""} for Windows</a>
      <a class="button" href="/download/#macos">Download for macOS &mdash; Apple Silicon beta</a>
      <a class="button" href="/core-plugin/">How it works</a>
      <a class="button" href="https://github.com/iamfatness/CoreVideo">View source</a>
    </div>
  </div>
</section>
<section class="link-grid" aria-label="What the plugin does">
  <div><strong>One source per participant</strong><span>Each Zoom participant becomes a separate OBS source at up to 1080p, with their audio on its own track &mdash; not a crop out of a gallery screenshot.</span></div>
  <div><strong>CoreVideo Tiles</strong><span>Arrange participant feeds in a styled gallery within one OBS source, with rounded corners, borders, and flexible layouts.</span></div>
  <div><strong>Active speaker &amp; spotlight</strong><span>Point a source at whoever is talking, at Zoom spotlight slot 1&hellip;N, or at a fixed guest with an automatic failover if they drop.</span></div>
  <div><strong>Screen share &amp; interpretation</strong><span>Subscribe to the live screen share, and pull existing Zoom interpretation audio channels in as dedicated sources.</span></div>
  <div><strong>ISO recording</strong><span>Record every assigned participant to an MP4 video file and a matching PCM WAV audio file, alongside the main program recording.</span></div>
  <div><strong>Active Speaker Director</strong><span>An automatic take with a hold time, a sensitivity threshold, an exclusion list, and a &ldquo;require video&rdquo; rule &mdash; so the cut follows the conversation without chattering.</span></div>
  <div><strong>Output profiles</strong><span>The Output Manager saves and reloads a whole assignment set, so last week&rsquo;s routing comes back on the next show instead of being rebuilt by hand.</span></div>
  <div><strong>Stream Deck &amp; Companion</strong><span>A Bitfocus Companion module (v5 or newer) picks the output and the participant <em>by name</em> from the live roster, so a button keeps working after somebody rejoins.</span></div>
  <div><strong>Remote control</strong><span>A TCP and OSC control surface drives joins, assignments, and recording from whatever already runs your show.</span></div>
</section>
<figure class="doc-image">
  <img src="/assets/obs-multiview.webp" alt="OBS Studio multiview: preview and program above four scenes built from individual Zoom participant sources - the host alone, the host with a reader, the host with the active speaker, and the active speaker full frame" loading="lazy">
  <figcaption>Every guest arrives as their own OBS source, so scenes, transitions, and the multiview all work the way they already do &mdash; this is stock OBS, cutting between Zoom participants.</figcaption>
</figure>
<section>
  <h2>Talkback, without leaving the meeting</h2>
  <p>Every show that runs on Zoom eventually needs a way to speak to one person without speaking to the room. The Zoom Meeting SDK carries a private talkback path alongside the meeting floor &mdash; 16 channels, up to 10 people each, and members duck under your voice rather than being muted &mdash; and both products here are built on it.</p>
  <p><a href="/zcomms/">ZComms</a> is the standalone answer: a full intercom desk with named talk keys, ALL CALL, latch, extern feeds from a multichannel interface, and breakout-aware routing. It ships today. The OBS plugin also has a <a href="/core-plugin/">Zoom Talkback dock</a> in the Windows v0.1.45-beta.1 build. It is absent from the recommended Windows v0.1.44 release and unavailable on macOS. Use the <a href="/download/">download page</a> to choose the appropriate release.</p>
</section>
<section class="link-grid products" aria-label="CoreVideo products">
  <a href="/download/"><span class="tier">Free &middot; OBS plugin</span><strong>CoreVideo for OBS</strong><span>The plugin on this page: Zoom participant video, audio, screen share, interpretation, and ISO recording as native sources in the OBS you already run. MIT licensed.</span></a>
  <a class="featured" href="/pro/"><span class="tier tier-premium">Premium &middot; Standalone app</span><strong>CoreVideo Pro</strong><span>Want the whole console instead of a plugin? Pro is the standalone studio being built around the same capture core: multi-scene production, participant management, recording, and streaming.</span></a>
  <a href="/zcomms/"><span class="tier">Standalone &middot; Talkback</span><strong>ZComms</strong><span>The intercom on its own. Named talk keys into Zoom&rsquo;s private talkback channels, extern feeds from a multichannel interface, and honest key states. Windows.</span></a>
</section>
<section class="link-grid" aria-label="CoreVideo resources">
  <a href="/documentation/"><strong>Plugin Docs</strong><span>Architecture, setup, control APIs, and operating notes.</span></a>
  <a href="/core-plugin/"><strong>Core Plugin Guide</strong><span>OBS workflows, participant routing, isolated audio, and ISO recording.</span></a>
  <a href="/zcomms/"><strong>ZComms</strong><span>Standalone talkback and IFB station for Zoom productions.</span></a>
  <a href="/pro/"><strong>CoreVideo Pro</strong><span>Standalone production app for live and recorded conversations.</span></a>
  <a href="/pro/documentation/"><strong>CoreVideo Pro Architecture</strong><span>Native media core, typed IPC, capture, AI direction, and outputs &mdash; with diagrams.</span></a>
  <a href="/terms/"><strong>Terms of Use</strong><span>Marketplace-ready usage terms and license requirements.</span></a>
  <a href="/privacy/"><strong>Privacy Policy</strong><span>Data processing, local storage, and third-party service details.</span></a>
  <a href="/support/"><strong>Support</strong><span>Issue reporting, troubleshooting, and common fixes.</span></a>
</section>`;
}

function proPageContent() {
  return `<section class="hero">
  <figure class="hero-media">
    <div class="console">
      <div class="console-bar"><span class="tally tally-air">On Air</span><span class="tally tally-pgm">Pgm</span><span class="tc">2160p60 &middot; 00:12:47:03</span></div>
      <div class="console-screen"><img src="/pro/images/corevideo-pro-studio.webp" alt="CoreVideo Pro production console: scene list, live program with multi-camera layout and lower-third, participant roster with roles and audio meters, and Take/Record/Stream controls"></div>
    </div>
  </figure>
  <div class="hero-copy">
    <p class="eyebrow">In development &middot; standalone studio</p>
    <h1>Produce polished live conversations from your Zoom calls.</h1>
    <p class="lede">CoreVideo Pro is the premium, all-in-one studio being built for live Zoom production: everything the CoreVideo plugin captures, plus multi-scene production, participant management, recording and multi-destination streaming, and an AI auto-director &mdash; in one standalone app for producers who want a dedicated console rather than a plugin.</p>
    <p class="lede"><strong>Pro is not released yet.</strong> There is no download and no price. This page and the architecture notes describe what is being built; the <a href="/download/">free OBS plugin</a> is the shipping product today, and <a href="/zcomms/">ZComms</a> is the shipping talkback desk.</p>
    <div class="hero-actions">
      <a class="button primary" href="/pro/documentation/">Read the architecture docs</a>
      <a class="button" href="/download/">Get the free OBS plugin</a>
    </div>
  </div>
</section>
<section class="link-grid" aria-label="CoreVideo Pro features">
  <div><strong>Multi-Scene Production</strong><span>Intro, interview, speaker-plus-slides, panel, and closing scene templates with Cut/Fade/Slide transitions and Take.</span></div>
  <div><strong>Participant Management</strong><span>Live Zoom roster with Host, Presenter, Panelist, and Guest roles, manual scene-slot assignment, and per-participant audio and video controls.</span></div>
  <div><strong>Streaming &amp; Recording</strong><span>Program and ISO recording, 1080p/4K output profiles, and multi-destination RTMP/NDI/SRT streaming with preflight checks.</span></div>
  <div><strong>AI Auto-Direct</strong><span>Magic Scene and Set &amp; Forget automatically recommend and take scene layouts from live Zoom activity, so a show can run itself.</span></div>
</section>
<section>
  <h2>The complete production studio</h2>
  <p>Pro is designed as a Zoom-native studio for Windows and macOS. Everything below is the capability set the product is being <strong>built toward</strong> &mdash; a target, not an inventory of what runs today. Individual items land, and get announced, one at a time.</p>
  <div class="feature-set">
    <div>
      <h3>Zoom capture &amp; media core</h3>
      <ul>
        <li>True Zoom Meeting SDK raw video and audio &mdash; no window or virtual-camera hacks</li>
        <li>Clean per-participant video, audio, and screen-share capture</li>
        <li>GPU compositor (Direct3D on Windows, Metal on macOS)</li>
        <li>Isolated capture process, automatic reconnect, and webinar mode</li>
      </ul>
    </div>
    <div>
      <h3>Scenes, templates &amp; switching</h3>
      <ul>
        <li>One-click professional templates: solo, interview, panel, screen-share, webinar</li>
        <li>Grid, speaker-focus, and picture-in-picture layouts with slot rules</li>
        <li>Program/preview workflow with explicit Take and Cut/Fade/Slide transitions</li>
        <li>Save and reload complete show presets</li>
      </ul>
    </div>
    <div>
      <h3>AI direction</h3>
      <ul>
        <li>Magic Scene builds a ready-to-stream show from the live call</li>
        <li>Set &amp; Forget auto-director switches on active speaker and screen share</li>
        <li>Role-aware logic &mdash; host for intros, presenter for shares, speaker for discussion</li>
        <li>Manual overrides always win, with one click back to auto</li>
      </ul>
    </div>
    <div>
      <h3>Participants &amp; framing</h3>
      <ul>
        <li>Every participant treated as a production-ready source, not a raw feed</li>
        <li>Face-aware auto-crop, centering, and manual zoom override</li>
        <li>Speaker holds to prevent rapid cutting, with graceful video-drop fallback</li>
        <li>Stable Host / Presenter / Panelist / Guest role overrides</li>
      </ul>
    </div>
    <div>
      <h3>Audio</h3>
      <ul>
        <li>Per-participant gain with smart auto-leveling and manual trim</li>
        <li>Per-source noise suppression, mute, and solo</li>
        <li>Master meter with limiter and clipping warnings</li>
        <li>A/V sync offset for local capture sources</li>
      </ul>
    </div>
    <div>
      <h3>Graphics, captions &amp; branding</h3>
      <ul>
        <li>Auto lower-thirds from Zoom name and role, with manual override</li>
        <li>Real-time program captions with speaker-name attribution</li>
        <li>Brand kit &mdash; logo, color, font, background &mdash; applied automatically</li>
        <li>Brand bug, banners, call-to-action overlays, and per-source chroma key</li>
      </ul>
    </div>
    <div>
      <h3>Local cameras (Blackmagic &amp; AJA)</h3>
      <ul>
        <li>Auto-detect DeckLink/UltraStudio and AJA Io/Kona, with hot-plug</li>
        <li>SDI/HDMI input selection with embedded or separate audio</li>
        <li>Local cameras as first-class sources, fillable into any slot</li>
        <li>Sub-100ms preview latency with signal-health monitoring</li>
      </ul>
    </div>
    <div>
      <h3>Recording &amp; streaming</h3>
      <ul>
        <li>Local MP4/MOV recording up to 4K at 30/60fps</li>
        <li>Multi-destination RTMP/NDI/SRT with YouTube, Twitch, and custom presets</li>
        <li>Per-participant ISO recording for clean guest capture</li>
        <li>Hardware encoding (NVENC, Quick Sync, AMF, VideoToolbox) with output preflight</li>
      </ul>
    </div>
  </div>
</section>
<section>
  <h2>Which one do I use?</h2>
  <p>Already run your shows in OBS? The free <a href="/core-plugin/">OBS plugin</a> brings clean Zoom participants in as native sources, and it is available today. Need to talk to one person without talking to the room? <a href="/zcomms/">ZComms</a> is the standalone talkback desk, also available today. CoreVideo Pro is the dedicated console being built on the same capture core &mdash; when it is ready to install, it will be on this page with a download button, not a paragraph.</p>
  <table>
    <thead><tr><th>Tier</th><th>Form factor</th><th>Best for</th></tr></thead>
    <tbody>
      <tr><td><strong>CoreVideo</strong> &mdash; Free, available now</td><td>OBS Studio plugin</td><td>Operators already running shows in OBS who want clean Zoom participants as native sources, ISO recording, an Active Speaker Director, and Stream Deck control.</td></tr>
      <tr><td><strong>ZComms</strong> &mdash; Available now</td><td>Standalone Windows app</td><td>Productions that need talkback: named keys into Zoom&apos;s private talkback channels, with its own operator position. See <a href="/zcomms/">ZComms</a>.</td></tr>
      <tr><td><strong>CoreVideo Pro</strong> &mdash; In development</td><td>Standalone app</td><td>Producers who want a dedicated studio &mdash; scenes, participants, outputs, recording, and AI auto-direct in one app. Not yet released.</td></tr>
    </tbody>
  </table>
</section>`;
}

function proDocsContent() {
  return `<h1>CoreVideo Pro Architecture</h1>
<p>CoreVideo Pro is a live-production studio for Windows and macOS, built around a native media core. It is <strong>in development and not released</strong>: this guide is a design document describing how the product is architected, in the present tense the way architecture is normally written, and not a claim that every stage below is finished. It describes end to end &mdash; how Zoom participants and local cameras are captured, how frames are composited and directed, and how program, ISO, and streaming outputs are produced. For the OBS plugin&apos;s internals, see the <a href="/core-plugin/">Core Plugin guide</a>.</p>
<h2>Design principles</h2>
<ul>
<li><strong>Native media core, web renderer.</strong> A C++ media core owns the real-time pipeline; the React/Vite renderer drives the UI and never touches media frames directly.</li>
<li><strong>Typed IPC contracts.</strong> The renderer and core talk over typed command/state contracts, so the host shell (Electron, Tauri, or a custom native shell) stays replaceable.</li>
<li><strong>Process isolation.</strong> The Zoom Meeting SDK runs in its own process, so an SDK crash cannot take down the operator console.</li>
<li><strong>Local-first.</strong> Capture, compositing, recording, and streaming all run on the operator&apos;s machine; nothing is routed through a third-party cloud.</li>
</ul>
<h2>System architecture</h2>
<figure class="doc-image"><img src="/pro/images/corevideo-pro-architecture.svg" alt="CoreVideo Pro system architecture: the React/Vite renderer over typed IPC, a native C++ media core, an isolated Zoom capture process plus local Blackmagic/AJA capture, and recording, ISO, and streaming outputs."></figure>
<p>The <strong>operator renderer</strong> presents the production console, scene and template editor, source and audio panels, and a keyboard command layer. It issues typed commands (assign slot, take, arm record, start stream) and receives typed state snapshots (feed health, output status, levels). It renders no media itself.</p>
<p>The <strong>native media core</strong> receives raw frames over a shared media bus, composites the active scene graph on the GPU, mixes audio, draws graphics and captions, and hands a program feed to the hardware encoder. The <strong>outputs</strong> stage records the program and per-guest ISOs and streams to one or more destinations.</p>
<h2>Capture and the media core</h2>
<p>Zoom media is captured through the Zoom Meeting SDK&apos;s raw video and audio APIs in a dedicated capture process &mdash; no window grabbing, virtual cameras, or display capture. Each participant becomes a clean video, audio, and screen-share source with a full data model: name, role, talking/mute/video state, spotlight, breakout room, and feed quality.</p>
<p>Local cameras from Blackmagic (DeckLink, UltraStudio) and AJA (Io, Kona) devices are detected on launch and on hot-plug, and appear as first-class sources alongside Zoom participants. Frames move from the capture process to the core over a shared media bus as GPU textures, so large frames are never copied through the IPC pipe. The GPU compositor renders with Direct3D 11/12 on Windows and Metal on macOS.</p>
<h2>Production pipeline</h2>
<figure class="doc-image"><img src="/pro/images/corevideo-pro-pipeline.svg" alt="CoreVideo Pro production pipeline: sources flow through capture and sync into a GPU scene-graph compositor fed by the AI director and audio mixer, producing a program feed that is encoded to recording, ISO, and streaming outputs."></figure>
<p>Sources are captured and synchronised, then routed into the scene-graph compositor. A scene is a template with typed slots (fixed, host, presenter, active speaker, screen share, gallery, fallback), assignment rules, and safe regions for lower-thirds and captions. Producers work in a program/preview model: stage a layout, then <strong>Take</strong> it to program with a Cut, Fade, or Slide transition.</p>
<p>The <strong>AI director</strong> watches the live call and feeds scene decisions into the compositor; the <strong>audio mixer</strong> levels every source and supplies the mixed bus to the program feed. The program feed is encoded once on hardware and fanned out to recording, ISO, and streaming.</p>
<h2>AI direction</h2>
<p><strong>Magic Scene</strong> inspects participant count, roles, screen-share, and the active speaker, picks a template, fills the slots, adds lower-thirds and captions, applies the brand kit, and produces a ready-to-stream scene set you can accept, regenerate, or edit. <strong>Set &amp; Forget</strong> then runs the show: it switches on active speaker and screen share, holds shots to avoid rapid cutting, reveals lower-thirds, and returns to the host or panel. Manual overrides always win, with one click back to automatic.</p>
<h2>Audio</h2>
<p>Each Zoom participant and local source has independent gain with smart auto-leveling and a manual trim on top, per-source noise suppression, and mute/solo. A master meter provides a limiter and clipping warnings, and an A/V sync offset aligns local capture with Zoom audio.</p>
<h2>Graphics, captions and branding</h2>
<p>Lower-thirds are generated automatically from each participant&apos;s Zoom name and role and can be overridden; they reveal and hide on cue and reposition to avoid collisions. Real-time program captions carry speaker attribution. A brand kit (logo, color, font, background) is applied automatically, alongside a brand bug, banners, call-to-action overlays, and per-source chroma key.</p>
<h2>Outputs</h2>
<p>CoreVideo Pro records the program to MP4/MOV (up to 4K, 30/60fps) and captures per-guest ISO feeds for clean re-edits. Streaming targets RTMP, NDI, and SRT with YouTube, Twitch, and custom presets and a multi-destination model that tracks armed/live state, bitrate, latency, and health per destination. Hardware encoders (NVENC, Quick Sync, AMF on Windows; VideoToolbox on macOS) keep CPU load low, and an output preflight blocks streaming when a destination is missing an endpoint, key, or compatible URL.</p>
<h2>Platform and shell</h2>
<p>The renderer is shell-agnostic: it runs inside whatever native host is present (Electron, Tauri, or a custom shell) and falls back to mock engines only for local development. Engine bundles are injected, so the UI swaps simulated engines for the native Zoom, media, and output implementations without importing mock singletons. The native media core stays the durable part of the product; the shell and renderer can evolve independently behind the typed IPC contracts.</p>`;
}

function zcommsPageContent() {
  return `<section class="hero">
  <figure class="hero-media">
    <div class="console">
      <div class="console-bar"><span class="tally tally-live">Key</span><span>All Call</span><span class="tc">16 ch &middot; 10 per ch</span></div>
      <div class="console-screen center"><div class="brand-lockup">${TALKBACK_MARK}<div class="wordmark">ZComms</div><div class="brand-sub">Talkback &amp; IFB station for Zoom</div></div></div>
    </div>
  </figure>
  <div class="hero-copy">
    <p class="eyebrow">Standalone Windows intercom station</p>
    <h1>Talk to one panelist. The room hears nothing.</h1>
    <p class="lede">ZComms is a talkback and IFB station for productions that run on Zoom. The panel is a grid of keys, and a key wears a person&apos;s name &mdash; hold it and your voice lands in that person&apos;s ear alone, while the meeting floor, the recording, and everyone else carry on unaware. It rides the Zoom Meeting SDK&apos;s own talkback channels: no virtual audio cable, no second machine, no bot account.</p>
    <div class="hero-actions">
      <a class="button primary" href="${ZCOMMS_RELEASES_LATEST}">Download for Windows</a>
      <a class="button" href="#how-it-works">How it works</a>
      <a class="button" href="${ZCOMMS_REPO}">View source</a>
    </div>
  </div>
</section>
<section class="link-grid" aria-label="What ZComms does">
  <div><strong>A named key per person</strong><span>Every talkback-capable participant gets a standing channel of their own, and the key wears their name. ALL CALL spans the panel; latch makes a press stick.</span></div>
  <div><strong>Private, not disruptive</strong><span>Talkback is its own path, so nothing you say reaches the meeting floor. Members duck under your voice while you are keyed and return to unity when you let go &mdash; ducked, not muted.</span></div>
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
  <p>The <a href="/core-plugin/">CoreVideo plugin</a> includes a Zoom Talkback dock in its Windows v0.1.45-beta.1 build, for operators keying talent from OBS. The recommended Windows v0.1.44 release has no talkback, and macOS talkback is unavailable. ZComms provides a separate operator position and feeds from a larger comms system.</p>
  <table>
    <thead><tr><th>Product</th><th>Form factor</th><th>Talkback</th></tr></thead>
    <tbody>
      <tr><td><strong>ZComms</strong></td><td>Standalone Windows app</td><td>The whole product &mdash; a full desk, extern feeds, and its own operator position. Shipping now.</td></tr>
      <tr><td><strong>CoreVideo</strong> for OBS</td><td>OBS Studio plugin</td><td>An intercom dock beside the video routing, for the operator already cutting the show. In development, unreleased.</td></tr>
    </tbody>
  </table>
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
function socialTags({ title, description, url, image }) {
  const tags = [
    ["og:type", "website"],
    ["og:site_name", SITE_NAME],
    ["og:title", title],
    ["og:description", description],
    ["og:image", image],
    ["og:image:alt", OG_IMAGE_ALT],
    ["og:image:width", "1200"],
    ["og:image:height", "630"],
    ["og:locale", "en_US"],
  ];
  if (url) tags.push(["og:url", url]);

  const twitter = [
    ["twitter:card", "summary_large_image"],
    ["twitter:title", title],
    ["twitter:description", description],
    ["twitter:image", image],
    ["twitter:image:alt", OG_IMAGE_ALT],
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
  const tagBase = v
    ? `https://github.com/iamfatness/CoreVideo/releases/download/v${v}`
    : null;
  const asset = (name) => `${tagBase}/${name}`;

  // Without a resolved version there are no version-stamped asset URLs to
  // build, so the page degrades to the always-current release listing rather
  // than printing links that 404.
  const primaryHref = v ? asset(`CoreVideo-Setup-v${v}.exe`) : RELEASES_LATEST;
  const primaryLabel = v
    ? `Download for Windows &mdash; v${v}`
    : "Download for Windows";

  const versionNote = v
    ? `<p class="lede">Latest release <strong>v${v}</strong> &mdash; Windows installer for OBS Studio 30 and newer. Free and open source under the MIT license.</p>`
    : `<p class="lede">Windows installer for OBS Studio 30 and newer. Free and open source under the MIT license.</p>`;

  const fileRows = v
    ? `<section class="link-grid" aria-label="Release files">
  <a href="${asset(`CoreVideo-Setup-v${v}.exe`)}"><strong>Windows installer (.exe)</strong><span>Recommended. Installs the plugin into your existing OBS Studio installation.</span></a>
  <a href="${asset(`CoreVideo-Windows-x64-v${v}.zip`)}"><strong>Portable ZIP (x64)</strong><span>Unpack into the OBS plugin directory yourself &mdash; for locked-down or portable OBS setups.</span></a>
  <a href="${asset(`CoreVideo-Setup-v${v}.exe.sha256`)}"><strong>Installer checksum</strong><span>SHA-256 for the installer, to verify the download before running it.</span></a>
  <a href="${asset(`CoreVideo-Windows-x64-v${v}.zip.sha256`)}"><strong>ZIP checksum</strong><span>SHA-256 for the portable archive.</span></a>
  <a href="${RELEASES_LATEST}"><strong>Companion module</strong><span>The Bitfocus Companion module (<code>corevideo-obs</code>) ships with each release. Requires Companion v5 or newer.</span></a>
</section>`
    : "";

  return `<section class="hero">
  <figure class="hero-media">
    <div class="console">
      <div class="console-bar"><span class="tally tally-live">Live</span><span>CoreVideo</span><span class="tc">${v ? `v${v}` : "latest"} &middot; x64</span></div>
      <div class="console-screen center"><div class="brand-lockup">${MULTIVIEW_MARK}<div class="wordmark">CoreVideo</div><div class="brand-sub">Free OBS Studio plugin</div></div></div>
    </div>
  </figure>
  <div class="hero-copy">
    <p class="eyebrow">Download</p>
    <h1>Install CoreVideo for OBS Studio.</h1>
    ${versionNote}
    <div class="hero-actions">
      <a class="button primary" href="${primaryHref}">${primaryLabel}</a>
      <a class="button" href="${RELEASES_LATEST}">Windows release notes</a>
    </div>
  </div>
</section>
${fileRows}
<p>Windows v0.1.44 remains the recommended release. The <a href="${MAC_RELEASE}">v0.1.45-beta.1 pre-release</a> also has Windows assets with the newer Zoom Talkback dock; treat those as beta builds.</p>
<section id="macos">
<h2>macOS &mdash; Apple Silicon beta v${MAC_VERSION}</h2>
<p>Developer ID signed, notarized by Apple, and stapled. The replacement installer has passed installation on two Macs and an OBS runtime test on the build Mac.</p>
<div class="hero-actions">
<a class="button primary" href="${MAC_INSTALLER}">Download macOS installer (.pkg)</a>
<a class="button" href="${MAC_INSTALLER}.sha256">SHA-256 checksum</a>
<a class="button" href="${MAC_RELEASE}">macOS release notes</a>
</div>
<p>If you downloaded this beta before September 6, 2026, download it again. The original macOS package and ZIP were withdrawn after signature errors.</p>
<p>Requires an Apple Silicon Mac (M1 or later). The replacement package was built against OBS Studio 32.2.1; use that version or newer for this beta. Intel Macs are not supported by this package.</p>
<ol>
<li>Close OBS, then open the downloaded <code>.pkg</code>.</li>
<li>Install for your user account. No administrator password is needed.</li>
<li>Reopen OBS, sign in to Zoom through CoreVideo, and join a meeting. Allow the requested permissions; the host must grant recording permission for media capture.</li>
</ol>
<p>For ISO recording, set a working FFmpeg executable in <strong>Zoom ISO Recorder</strong> and test it before recording. The macOS package does not bundle FFmpeg.</p>
<p>Talkback is Windows-only. For your own screen, use OBS's macOS Screen Capture source; CoreVideo receives other participants' shares.</p>
<p>To verify the download, save the installer and checksum in the same folder and run:</p>
<pre><code>shasum -a 256 -c CoreVideo-Setup-v${MAC_VERSION}.pkg.sha256</code></pre>
</section>
<h2>Windows requirements</h2>
<ul>
<li><strong>OBS Studio 30 or newer</strong>, 64-bit.</li>
<li><strong>Windows 10 or Windows 11</strong> (x64). For macOS, use the <a href="#macos">Apple Silicon beta installer above</a>. Linux requires a source build.</li>
<li><strong>A Zoom account you can sign in with.</strong> CoreVideo joins meetings through the Zoom Meeting SDK, so the video quality and number of simultaneous feeds you get follow your Zoom account entitlements.</li>
<li><strong>Downstream bandwidth</strong> for the feeds you subscribe to &mdash; roughly 4-6 Mbps per 1080p participant. Standard accounts are typically capped around 30 Mbps incoming; Enhanced Media / HBM raises that to roughly 100 Mbps.</li>
</ul>
<h2>Install on Windows</h2>
<ol>
<li>Close OBS Studio.</li>
<li>Run the installer and point it at your OBS installation directory if it is not detected automatically.</li>
<li>Start OBS and open <strong>Tools -&gt; Zoom Plugin Settings</strong> to sign in, then <strong>Docks -&gt; Zoom Control</strong> to join a meeting.</li>
<li>Add a <strong>CoreVideo Participant</strong> source to any scene, then assign it to a participant, the active speaker, a spotlight slot, or the screen share.</li>
</ol>
<p>The Windows installer is <strong>not code-signed yet</strong>, so Windows SmartScreen warns the first time you run it &mdash; &quot;More info&quot;, then &quot;Run anyway&quot;. Verify the SHA-256 below if you would rather check the file than trust the dialog.</p>
<p>CoreVideo is in public beta. The <a href="/core-plugin/">Core Plugin Guide</a> walks through participant routing, isolated audio, and ISO recording in detail, and <a href="/support/">Support</a> covers log collection and reporting a bug.</p>
<h2>Verify your download</h2>
<p>Each release ships a SHA-256 file alongside the binary. On Windows, compare the hashes with PowerShell:</p>
<pre><code class="language-powershell">Get-FileHash .\\CoreVideo-Setup-${v ? `v${v}` : "vX.Y.Z"}.exe -Algorithm SHA256</code></pre>
<h2>Building from source</h2>
<p>CoreVideo is MIT-licensed and builds from source on Windows, macOS, and Linux. See the <a href="https://github.com/iamfatness/CoreVideo">repository</a> for the toolchain requirements and CMake options, including hardware-accelerated colour conversion.</p>`;
}

// Structured data for the free plugin. SoftwareApplication is what earns the
// download-style rich result; the price-0 offer is what marks it free.
function pluginJsonLd() {
  const app = {
    "@type": "SoftwareApplication",
    name: "CoreVideo",
    alternateName: "CoreVideo OBS Plugin",
    applicationCategory: "MultimediaApplication",
    applicationSubCategory: "OBS Studio plugin",
    operatingSystem: "Windows 10, Windows 11",
    description:
      "OBS Studio plugin that captures Zoom meeting participants as native OBS sources - per-participant video and audio, screen share, interpretation audio, and ISO recording - using the Zoom Meeting SDK rather than NDI, a virtual camera, or screen capture.",
    url: `${PRIMARY_ORIGIN}/`,
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

  return {
    "@context": "https://schema.org",
    "@graph": [
      app,
      {
        "@type": "WebSite",
        name: SITE_NAME,
        url: `${PRIMARY_ORIGIN}/`,
      },
    ],
  };
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
    image: OG_IMAGE,
  };
}

function layout(page, content, options = {}) {
  const nav = [
    ["Home", "/"],
    ["CoreVideo Pro", "/pro/"],
    ["ZComms", "/zcomms/"],
    ["Pro Docs", "/pro/documentation/"],
    ["Plugin Docs", "/documentation/"],
    ["Core Plugin", "/core-plugin/"],
    ["Download", "/download/"],
    ["Terms", "/terms/"],
    ["Privacy", "/privacy/"],
    ["Support", "/support/"],
  ];

  const footerText = options.footerText ??
    "CoreVideo is an independent product and is not affiliated with Zoom Video Communications, Inc.";

  const here = options.canonical ? options.canonical.replace(PRIMARY_ORIGIN, "") : null;

  // Search results and social cards show the title verbatim, so a page that
  // sets seoTitle owns the whole string; everything else keeps the
  // "<page> | CoreVideo" suffix convention.
  const fullTitle = page.seoTitle ?? `${page.title} | ${SITE_NAME}`;
  const ogImage = page.ogImage ?? OG_IMAGE;

  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(fullTitle)}</title>
  <meta name="description" content="${escapeHtml(page.description)}">
  <meta name="theme-color" content="#0a0b0c">
  <link rel="icon" href="/favicon.svg">
  <link rel="preload" href="/assets/fonts/SpaceGrotesk-var.woff2" as="font" type="font/woff2" crossorigin>
  <link rel="preload" href="/assets/fonts/IBMPlexMono-Regular.woff2" as="font" type="font/woff2" crossorigin>
${options.canonical ? `  <link rel="canonical" href="${escapeHtml(options.canonical)}">\n` : ""}${socialTags({ title: fullTitle, description: page.description, url: options.canonical, image: ogImage })}
  <link rel="stylesheet" href="/assets/site.css">
${options.jsonLd ? jsonLdBlock(options.jsonLd) : ""}</head>
<body class="${options.home ? "home-page" : "document-page"}">
  <header class="site-header">
    <a class="brand" href="/">${MULTIVIEW_MARK}<span>CoreVideo</span></a>
    <nav>${nav.map(([label, href]) => `<a href="${href}"${here && href === here ? ' aria-current="page"' : ""}>${label}</a>`).join("")}</nav>
  </header>
  <main class="content">
    ${content}
  </main>
  <footer class="site-footer">
    <span>${footerText}</span>
  </footer>
</body>
</html>
`;
}

function writeText(relativePath, content) {
  const filePath = path.join(outDir, relativePath);
  ensureDir(filePath);
  fs.writeFileSync(filePath, content, "utf8");
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
    jsonLd: isHome ? pluginJsonLd() : undefined,
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
      description:
        "A standalone Windows studio for live Zoom production: scenes, participant management, recording, streaming and AI auto-direct. In development, not yet released.",
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
        "CoreVideo Pro architecture: native media core, typed IPC, isolated Zoom capture, Blackmagic/AJA input, GPU compositing, AI direction, and ISO/streaming outputs.",
    },
    proDocsContent(),
    {
      canonical: canonicalUrl("pro/documentation/index.html"),
      footerText:
        "CoreVideo and CoreVideo Pro are independent products and are not affiliated with Zoom Video Communications, Inc.",
    },
  ),
);

// ZComms landing page. ZComms is its own product with its own repository and
// its own release cadence; the site carries it because it is the third thing
// the same audience buys into, alongside the plugin and Pro.
writeText(
  "zcomms/index.html",
  layout(
    {
      title: "ZComms",
      seoTitle: "ZComms - Zoom Talkback & Intercom Station for Live Production",
      description:
        "Standalone Windows talkback and IFB station for Zoom: named talk keys, 16 private channels, extern feeds from a multichannel interface, breakout-aware.",
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

// Shared site imagery: the Open Graph card plus the two product screenshots the
// plugin pages lead with. Missing files are skipped rather than failing the
// build, matching how the logo and Pro screenshot are handled below.
for (const name of [
  "corevideo-share.jpg",
  "obs-multiview.webp",
  "dynamic-gallery.webp",
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
      "\n</head>",
  );
if (!docsHtml.includes('rel="canonical"')) {
  throw new Error(
    "docs/index.html has no </head> to inject SEO tags into; the documentation " +
      "page would ship without a canonical, description, or social card.",
  );
}
writeText("documentation/index.html", docsHtml);
writeText("docs/index.html", docsHtml);

writeText(
  "assets/site.css",
  fs.readFileSync(path.join(siteAssetsDir, "site.css"), "utf8"),
);

// Docs-page script (mermaid theme init + sidebar highlighting). Must be an
// external self-hosted file: the worker's CSP has script-src 'self' + jsdelivr
// with no 'unsafe-inline', so inline scripts are silently blocked in
// production (that is how the mermaid theme config never applied for months).
writeText(
  "assets/docs-init.js",
  fs.readFileSync(path.join(siteAssetsDir, "docs-init.js"), "utf8"),
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
