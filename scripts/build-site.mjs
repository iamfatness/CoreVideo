import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const wikiDir = path.join(root, "wiki");
const docsDir = path.join(root, "docs");
const siteAssetsDir = path.join(root, "site-assets");
const outDir = path.join(root, "public");

const pages = [
  {
    source: "Home.md",
    title: "CoreVideo",
    description: "CoreVideo documentation, policies, support, and Zoom Marketplace resources.",
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
  return escapeHtml(value)
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/_([^_]+)_/g, "<em>$1</em>")
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_match, label, href) => {
      const resolved = resolveHref(href);
      return `<a href="${escapeHtml(resolved)}">${label}</a>`;
    });
}

function resolveHref(href) {
  if (href.startsWith("https://iamfatness.github.io/CoreVideo/#"))
    return `/documentation/${href.slice("https://iamfatness.github.io/CoreVideo/#".length)}`;
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

function normalizeText(value) {
  return value
    .replaceAll("â€”", "-")
    .replaceAll("â†’", "->")
    .replaceAll("â€¦", "...")
    .replaceAll("ðŸ“–", "")
    .replaceAll("behaviour", "behavior")
    .replaceAll("https://iamfatness.github.io/CoreVideo/", "/documentation/")
    .replaceAll("https://iamfatness.github.io/CoreVideo", "/documentation");
}

function homeContent() {
  return `<section class="hero">
  <figure class="hero-media">
    <img class="hero-logo" src="/assets/corevideo-logo.jpg" alt="CoreVideo">
  </figure>
  <div class="hero-copy">
    <p class="eyebrow">OBS Studio plugin for Zoom Meeting SDK production workflows</p>
    <h1>Live Zoom video, audio, screen share, and interpretation inside OBS.</h1>
    <p class="lede">CoreVideo provides raw participant media routing for broadcast, recording, and ISO-style production workflows while keeping processing local to the operator machine.</p>
    <div class="hero-actions">
      <a class="button primary" href="/documentation/">Read documentation</a>
      <a class="button" href="https://github.com/iamfatness/CoreVideo">View source</a>
    </div>
  </div>
</section>
<section class="link-grid" aria-label="CoreVideo resources">
  <a href="/documentation/"><strong>Documentation</strong><span>Architecture, setup, control APIs, and operating notes.</span></a>
  <a href="/terms/"><strong>Terms of Use</strong><span>Marketplace-ready usage terms and license requirements.</span></a>
  <a href="/privacy/"><strong>Privacy Policy</strong><span>Data processing, local storage, and third-party service details.</span></a>
  <a href="/support/"><strong>Support</strong><span>Issue reporting, troubleshooting, and common fixes.</span></a>
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
  const lines = markdown.replace(/\r\n/g, "\n").split("\n");
  const html = [];
  let list = [];
  let paragraph = [];
  let quote = [];
  let table = [];

  const flushParagraph = () => {
    if (!paragraph.length) return;
    html.push(`<p>${inlineMarkdown(paragraph.join(" "))}</p>`);
    paragraph = [];
  };
  const flushList = () => {
    if (!list.length) return;
    html.push(`<ul>${list.map((item) => `<li>${inlineMarkdown(item)}</li>`).join("")}</ul>`);
    list = [];
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
    if (!line.trim()) {
      flushAll();
      continue;
    }

    if (line.startsWith("|")) {
      flushParagraph();
      flushList();
      flushQuote();
      table.push(line);
      continue;
    }
    flushTable();

    if (line === "---") {
      flushAll();
      html.push("<hr>");
      continue;
    }

    const heading = /^(#{1,6})\s+(.+)$/.exec(line);
    if (heading) {
      flushAll();
      const level = heading[1].length;
      html.push(`<h${level}>${inlineMarkdown(heading[2])}</h${level}>`);
      continue;
    }

    if (line.startsWith("- ")) {
      flushParagraph();
      flushQuote();
      list.push(line.slice(2));
      continue;
    }

    if (/^\d+\.\s+/.test(line)) {
      flushParagraph();
      flushQuote();
      list.push(line.replace(/^\d+\.\s+/, ""));
      continue;
    }

    if (line.startsWith("> ")) {
      flushParagraph();
      flushList();
      quote.push(line.slice(2));
      continue;
    }

    paragraph.push(line.trim());
  }
  flushAll();
  return html.join("\n");
}

function layout(page, content, options = {}) {
  const nav = [
    ["Home", "/"],
    ["Documentation", "/documentation/"],
    ["Terms", "/terms/"],
    ["Privacy", "/privacy/"],
    ["Support", "/support/"],
    ["GitHub", "https://github.com/iamfatness/CoreVideo"],
  ];

  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(page.title)} | CoreVideo</title>
  <meta name="description" content="${escapeHtml(page.description)}">
  <link rel="stylesheet" href="/assets/site.css">
</head>
<body class="${options.home ? "home-page" : "document-page"}">
  <header class="site-header">
    <a class="brand" href="/"><span class="brand-mark" aria-hidden="true"></span><span>CoreVideo</span></a>
    <nav>${nav.map(([label, href]) => `<a href="${href}">${label}</a>`).join("")}</nav>
  </header>
  <main class="content">
    ${content}
  </main>
  <footer class="site-footer">
    <span>CoreVideo is an independent open-source project and is not affiliated with Zoom Video Communications, Inc.</span>
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
  });
  writeText(page.output, html);
  for (const alias of page.aliases ?? []) {
    writeText(alias, html);
  }
}

const logoSource = path.join(siteAssetsDir, "corevideo-logo.jpg");
if (fs.existsSync(logoSource)) {
  const logoTarget = path.join(outDir, "assets", "corevideo-logo.jpg");
  ensureDir(logoTarget);
  fs.copyFileSync(logoSource, logoTarget);
}

const docsHtml = fs.readFileSync(path.join(docsDir, "index.html"), "utf8")
  .replaceAll("iamfatness.github.io/CoreVideo", publicDocumentationUrl
    ? new URL("/documentation", publicDocumentationUrl).host + "/documentation"
    : "CoreVideo documentation")
  .replaceAll("https://iamfatness.github.io/CoreVideo/", "/documentation/");
writeText("documentation/index.html", docsHtml);
writeText("docs/index.html", docsHtml);

writeText("assets/site.css", `:root {
  color-scheme: dark;
  --bg: #070914;
  --bg-2: #101528;
  --panel: rgba(14, 18, 35, 0.88);
  --panel-strong: #11172c;
  --text: #f7faff;
  --muted: #9daac2;
  --line: rgba(125, 239, 255, 0.18);
  --cyan: #22e7e8;
  --blue: #2aa8ff;
  --cyan-soft: rgba(34, 231, 232, 0.14);
}
* { box-sizing: border-box; }
html { min-height: 100%; }
body {
  margin: 0;
  min-height: 100%;
  font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background:
    radial-gradient(circle at 28% 18%, rgba(34, 231, 232, 0.16), transparent 26rem),
    radial-gradient(circle at 82% 72%, rgba(42, 168, 255, 0.12), transparent 30rem),
    linear-gradient(145deg, var(--bg), var(--bg-2));
  color: var(--text);
  line-height: 1.6;
}
body::before {
  content: "";
  position: fixed;
  inset: 0;
  pointer-events: none;
  background:
    linear-gradient(rgba(255,255,255,0.025) 1px, transparent 1px),
    linear-gradient(90deg, rgba(255,255,255,0.025) 1px, transparent 1px);
  background-size: 42px 42px;
  mask-image: linear-gradient(to bottom, black, transparent 78%);
}
a { color: var(--cyan); text-decoration-thickness: 1px; text-underline-offset: 3px; }
.site-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 24px;
  padding: 18px clamp(20px, 5vw, 56px);
  border-bottom: 1px solid var(--line);
  background: rgba(7, 9, 20, 0.82);
  backdrop-filter: blur(16px);
  position: sticky;
  top: 0;
  z-index: 10;
}
.brand {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  color: var(--text);
  font-weight: 750;
  font-size: 20px;
  text-decoration: none;
}
.brand-mark {
  position: relative;
  display: inline-block;
  width: 32px;
  height: 32px;
  border-radius: 50%;
  background: linear-gradient(135deg, var(--cyan), var(--blue));
  box-shadow: 0 0 24px rgba(34, 231, 232, 0.35);
}
.brand-mark::before {
  content: "";
  position: absolute;
  inset: 7px 8px;
  background: #07101c;
  border-radius: 50%;
}
.brand-mark::after {
  content: "";
  position: absolute;
  left: 13px;
  top: 9px;
  width: 0;
  height: 0;
  border-top: 7px solid transparent;
  border-bottom: 7px solid transparent;
  border-left: 11px solid var(--cyan);
  filter: drop-shadow(0 0 6px rgba(34, 231, 232, 0.75));
}
nav {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  flex-wrap: wrap;
  gap: 16px;
}
nav a {
  color: var(--muted);
  font-size: 14px;
  text-decoration: none;
}
nav a:hover { color: var(--text); }
.content {
  width: min(980px, calc(100% - 40px));
  margin: 42px auto 56px;
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 8px;
  box-shadow: 0 24px 80px rgba(0, 0, 0, 0.42);
  padding: clamp(24px, 5vw, 52px);
  position: relative;
}
.home-page .content {
  width: min(1120px, calc(100% - 40px));
  background: transparent;
  border: 0;
  box-shadow: none;
  padding: 0;
}
.hero {
  display: grid;
  grid-template-columns: minmax(0, 1.02fr) minmax(360px, 0.98fr);
  align-items: center;
  gap: clamp(30px, 5vw, 68px);
  padding: clamp(28px, 6vw, 70px);
  border: 1px solid var(--line);
  border-radius: 8px;
  background:
    linear-gradient(135deg, rgba(9, 13, 27, 0.96), rgba(12, 17, 34, 0.82)),
    radial-gradient(circle at 12% 20%, rgba(34, 231, 232, 0.16), transparent 22rem);
  box-shadow: 0 24px 90px rgba(0, 0, 0, 0.5);
}
.hero-copy { max-width: 620px; }
.hero-media {
  margin: 0;
  position: relative;
}
.hero-media::before {
  content: "";
  position: absolute;
  inset: 14% 18% auto auto;
  width: 42%;
  aspect-ratio: 1;
  border-radius: 50%;
  background: rgba(34, 231, 232, 0.18);
  filter: blur(42px);
}
.hero-logo {
  display: block;
  position: relative;
  width: 100%;
  height: auto;
  border-radius: 8px;
  box-shadow:
    0 22px 80px rgba(0, 0, 0, 0.42),
    0 0 0 1px rgba(125, 239, 255, 0.12);
}
.eyebrow {
  color: var(--cyan);
  margin: 0 0 12px;
  font-size: 13px;
  font-weight: 750;
  letter-spacing: 0.08em;
  text-transform: uppercase;
}
.hero h1 {
  margin: 0 0 18px;
  max-width: 820px;
  font-size: clamp(36px, 6vw, 70px);
  letter-spacing: 0;
}
.lede {
  color: #d5def0;
  max-width: 660px;
  font-size: 18px;
}
.hero-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  margin-top: 28px;
}
.button {
  display: inline-flex;
  align-items: center;
  min-height: 44px;
  padding: 10px 18px;
  border: 1px solid var(--line);
  border-radius: 6px;
  color: var(--text);
  background: rgba(255,255,255,0.06);
  text-decoration: none;
}
.button.primary {
  color: #06111a;
  border-color: transparent;
  background: linear-gradient(135deg, var(--cyan), var(--blue));
  font-weight: 750;
}
.link-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 14px;
  margin-top: 18px;
}
.link-grid a {
  min-height: 160px;
  padding: 20px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: rgba(17, 23, 44, 0.86);
  text-decoration: none;
}
.link-grid strong {
  display: block;
  color: var(--text);
  font-size: 18px;
  margin-bottom: 8px;
}
.link-grid span { color: var(--muted); }
h1, h2, h3, h4 { line-height: 1.2; margin: 1.6em 0 0.55em; letter-spacing: 0; }
h1 { margin-top: 0; font-size: clamp(34px, 5vw, 52px); }
h2 { font-size: 26px; border-top: 1px solid var(--line); padding-top: 28px; }
h3 { font-size: 21px; color: #dcecff; }
p, ul, table, blockquote { margin: 0 0 18px; }
p, li, td { color: #d6deee; }
ul { padding-left: 24px; }
code {
  background: rgba(34, 231, 232, 0.1);
  border: 1px solid rgba(34, 231, 232, 0.16);
  border-radius: 4px;
  padding: 0.1em 0.35em;
  font-size: 0.92em;
  color: #bafcff;
}
table {
  width: 100%;
  border-collapse: collapse;
  font-size: 15px;
}
th, td {
  text-align: left;
  vertical-align: top;
  border: 1px solid var(--line);
  padding: 10px 12px;
}
th { background: rgba(34, 231, 232, 0.08); color: var(--text); }
blockquote {
  border-left: 4px solid var(--cyan);
  padding: 12px 18px;
  background: var(--cyan-soft);
  color: #e7fbff;
}
hr {
  border: 0;
  border-top: 1px solid var(--line);
  margin: 30px 0;
}
.site-footer {
  color: var(--muted);
  font-size: 13px;
  padding: 24px clamp(20px, 5vw, 56px) 40px;
  text-align: center;
}
@media (max-width: 900px) {
  .hero {
    grid-template-columns: 1fr;
  }
  .hero-media { order: -1; }
  .link-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
@media (max-width: 700px) {
  .site-header { align-items: flex-start; flex-direction: column; }
  nav { justify-content: flex-start; }
  .content { margin-top: 24px; }
  .hero { min-height: 0; }
  .link-grid { grid-template-columns: 1fr; }
}
`);

writeText("_redirects", `/terms-of-use /terms/ 301
/Terms-of-Use /terms/ 301
/privacy-policy /privacy/ 301
/Privacy-Policy /privacy/ 301
/Support /support/ 301
/docs /documentation/ 301
`);

console.log(`Built CoreVideo site in ${path.relative(root, outDir)}`);
