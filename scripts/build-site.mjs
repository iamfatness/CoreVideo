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

function canonicalUrl(output) {
  const pathPart = ("/" + output).replace(/index\.html$/, "");
  return PRIMARY_ORIGIN + (pathPart || "/");
}

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
    description: "CoreVideo: the OBS plugin for live Zoom video, and CoreVideo Pro, a standalone app for producing online conversations.",
    output: "index.html",
  },
  {
    source: "Terms-of-Use.md",
    title: "Terms of Use",
    description: "Terms of Use for the CoreVideo OBS Studio plugin.",
    output: "terms/index.html",
    aliases: ["terms-of-use/index.html", "Terms-of-Use/index.html"],
  },
  {
    source: "Privacy-Policy.md",
    title: "Privacy Policy",
    description: "Privacy Policy for the CoreVideo OBS Studio plugin.",
    output: "privacy/index.html",
    aliases: ["privacy-policy/index.html", "Privacy-Policy/index.html"],
  },
  {
    source: "Support.md",
    title: "Support",
    description: "Support resources for the CoreVideo OBS Studio plugin.",
    output: "support/index.html",
    aliases: ["Support/index.html"],
  },
];

const markdownPages = [
  {
    source: path.join(docsDir, "ZOOM_MARKETPLACE_OAUTH.md"),
    title: "OAuth Setup",
    description: "Zoom Marketplace OAuth setup for CoreVideo.",
    output: "oauth/index.html",
  },
  {
    source: path.join(docsDir, "CORE_PLUGIN_FUNCTIONALITY.md"),
    title: "CoreVideo (OBS Plugin)",
    description: "CoreVideo OBS plugin workflows, control examples, and ISO recording.",
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
      <div class="console-bar"><span class="tally tally-live">Live</span><span>Multiview 01</span><span class="tc">1080p60 &middot; 00:00:00:00</span></div>
      <div class="console-screen center"><img class="hero-logo" src="/assets/corevideo-logo.jpg" alt="CoreVideo"></div>
    </div>
  </figure>
  <div class="hero-copy">
    <p class="eyebrow">Live Zoom production &mdash; from free plugin to full studio</p>
    <h1>Produce broadcast-grade Zoom shows in one studio app.</h1>
    <p class="lede">CoreVideo Pro is the complete standalone production studio for live Zoom conversations &mdash; scenes, participants, AI direction, recording, and streaming in a single premium app. Already working in OBS? The free CoreVideo plugin brings the same clean participant capture to your existing rig.</p>
    <div class="hero-actions">
      <a class="button primary" href="/pro/">Explore CoreVideo Pro</a>
      <a class="button" href="/core-plugin/">Free OBS plugin</a>
      <a class="button" href="/documentation/">Plugin Docs</a>
    </div>
  </div>
</section>
<section class="link-grid products" aria-label="CoreVideo products">
  <a class="featured" href="/pro/"><span class="tier tier-premium">Premium &middot; Standalone app</span><strong>CoreVideo Pro</strong><span>The complete production studio for live Zoom conversations: multi-scene production, participant management, AI auto-direct, recording, and multi-destination streaming in one polished app.</span></a>
  <a href="/core-plugin/"><span class="tier">Free &middot; OBS plugin</span><strong>CoreVideo (OBS Plugin)</strong><span>The free building block: clean Zoom participant video, audio, screen share, and ISO recording as native sources inside OBS Studio.</span></a>
</section>
<section class="link-grid" aria-label="CoreVideo resources">
  <a href="/documentation/"><strong>Plugin Docs</strong><span>Architecture, setup, control APIs, and operating notes.</span></a>
  <a href="/core-plugin/"><strong>Core Plugin Guide</strong><span>OBS workflows, participant routing, isolated audio, and ISO recording.</span></a>
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
    <p class="eyebrow">The complete CoreVideo production studio</p>
    <h1>Produce polished live conversations from your Zoom calls.</h1>
    <p class="lede">CoreVideo Pro is the premium, all-in-one studio for live Zoom production: everything the CoreVideo plugin captures, plus multi-scene production, participant management, recording and multi-destination streaming, and an AI auto-director &mdash; in one standalone app for producers who want a dedicated, ready-to-go console.</p>
    <div class="hero-actions">
      <a class="button primary" href="/pro/documentation/">Read the architecture docs</a>
      <a class="button" href="/core-plugin/">Compare with the OBS plugin</a>
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
  <p>CoreVideo Pro is a cross-platform (macOS and Windows) Zoom-native studio. Here is the full capability set the product is built toward &mdash; everything you need to take a Zoom call to a finished, broadcast-quality show:</p>
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
  <h2>Free plugin or full studio?</h2>
  <p>CoreVideo fits both workflows. Already run your shows in OBS? The free <a href="/core-plugin/">OBS plugin</a> brings clean Zoom participants in as native sources. Want a more polished, dedicated way to produce a show? CoreVideo Pro is the premium standalone studio that includes that same Zoom capture and adds the full production layer.</p>
  <table>
    <thead><tr><th>Tier</th><th>Form factor</th><th>Best for</th></tr></thead>
    <tbody>
      <tr><td><strong>CoreVideo Pro</strong> &mdash; Premium</td><td>Standalone app</td><td>Producers who want a dedicated, ready-to-go studio &mdash; scenes, participants, outputs, recording, and AI auto-direct in one polished app.</td></tr>
      <tr><td><strong>CoreVideo</strong> &mdash; Free</td><td>OBS Studio plugin</td><td>Operators already running shows in OBS who want clean Zoom participants as native sources, ISO recording, and an Active Speaker Director.</td></tr>
    </tbody>
  </table>
</section>`;
}

function proDocsContent() {
  return `<h1>CoreVideo Pro Architecture</h1>
<p>CoreVideo Pro is a cross-platform (macOS and Windows) live-production studio built around a native media core. This guide describes how the product is architected end to end &mdash; how Zoom participants and local cameras are captured, how frames are composited and directed, and how program, ISO, and streaming outputs are produced. For the OBS plugin&apos;s internals, see the <a href="/core-plugin/">Core Plugin guide</a>.</p>
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
        html.push(`<pre><code${languageClass}>${escapeHtml(codeBlock.lines.join("\n"))}</code></pre>`);
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

function layout(page, content, options = {}) {
  const nav = [
    ["Home", "/"],
    ["CoreVideo Pro", "/pro/"],
    ["Pro Docs", "/pro/documentation/"],
    ["Plugin Docs", "/documentation/"],
    ["Core Plugin", "/core-plugin/"],
    ["OAuth", "/oauth/"],
    ["Terms", "/terms/"],
    ["Privacy", "/privacy/"],
    ["Support", "/support/"],
  ];

  const footerText = options.footerText ??
    "CoreVideo is an independent product and is not affiliated with Zoom Video Communications, Inc.";

  const here = options.canonical ? options.canonical.replace(PRIMARY_ORIGIN, "") : null;

  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(page.title)} | CoreVideo</title>
  <meta name="description" content="${escapeHtml(page.description)}">
  <meta name="theme-color" content="#0a0b0c">
  <link rel="icon" href="/favicon.svg">
  <link rel="preload" href="/assets/fonts/SpaceGrotesk-var.woff2" as="font" type="font/woff2" crossorigin>
  <link rel="preload" href="/assets/fonts/IBMPlexMono-Regular.woff2" as="font" type="font/woff2" crossorigin>
${options.canonical ? `  <link rel="canonical" href="${escapeHtml(options.canonical)}">\n` : ""}  <link rel="stylesheet" href="/assets/site.css">
</head>
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

fs.rmSync(outDir, { recursive: true, force: true });

for (const page of pages) {
  const markdown = normalizeText(
    fs.readFileSync(path.join(wikiDir, page.source), "utf8"),
  );
  const isHome = page.output === "index.html";
  const html = layout(page, isHome ? homeContent() : markdownToHtml(markdown), {
    home: isHome,
    canonical: canonicalUrl(page.output),
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

// CoreVideo Pro landing page
writeText(
  "pro/index.html",
  layout(
    {
      title: "CoreVideo Pro",
      description:
        "CoreVideo Pro: a cross-platform (macOS and Windows) standalone app for producing high-quality online conversations with multi-scene production, participant management, streaming, recording, and AI auto-direct.",
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
        "CoreVideo Pro architecture: native media core, typed IPC renderer, isolated Zoom capture, local Blackmagic/AJA capture, GPU compositing, AI direction, and recording/ISO/streaming outputs.",
    },
    proDocsContent(),
    {
      canonical: canonicalUrl("pro/documentation/index.html"),
      footerText:
        "CoreVideo and CoreVideo Pro are independent products and are not affiliated with Zoom Video Communications, Inc.",
    },
  ),
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

const docsHtml = fs.readFileSync(path.join(docsDir, "index.html"), "utf8")
  .replaceAll("iamfatness.github.io/CoreVideo", publicDocumentationUrl
    ? new URL("/documentation", publicDocumentationUrl).host + "/documentation"
    : "CoreVideo documentation")
  .replaceAll("https://iamfatness.github.io/CoreVideo/", "/documentation/")
  .replaceAll('href="ZOOM_MARKETPLACE_OAUTH.md"', 'href="/oauth/"')
  .replace(
    "</head>",
    `  <link rel="canonical" href="${canonicalUrl("documentation/index.html")}">\n</head>`,
  );
writeText("documentation/index.html", docsHtml);
writeText("docs/index.html", docsHtml);

writeText(
  "assets/site.css",
  fs.readFileSync(path.join(siteAssetsDir, "site.css"), "utf8"),
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
/ZOOM_MARKETPLACE_OAUTH.md /oauth/ 301
/docs/ZOOM_MARKETPLACE_OAUTH.md /oauth/ 301
`);
writeText("CNAME", "corevideo.io\n");

console.log(`Built CoreVideo site in ${path.relative(root, outDir)}`);
