import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const wikiDir = path.join(root, "wiki");
const docsDir = path.join(root, "docs");
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

function layout(page, content) {
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
<body>
  <header class="site-header">
    <a class="brand" href="/">CoreVideo</a>
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
  const html = layout(page, markdownToHtml(markdown));
  writeText(page.output, html);
  for (const alias of page.aliases ?? []) {
    writeText(alias, html);
  }
}

const docsHtml = fs.readFileSync(path.join(docsDir, "index.html"), "utf8")
  .replaceAll("iamfatness.github.io/CoreVideo", publicDocumentationUrl
    ? new URL("/documentation", publicDocumentationUrl).host + "/documentation"
    : "CoreVideo documentation")
  .replaceAll("https://iamfatness.github.io/CoreVideo/", "/documentation/");
writeText("documentation/index.html", docsHtml);
writeText("docs/index.html", docsHtml);

writeText("assets/site.css", `:root {
  color-scheme: light;
  --bg: #f7f8fb;
  --panel: #ffffff;
  --text: #172033;
  --muted: #5f6f86;
  --line: #d9e0ea;
  --accent: #0b5cff;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background: var(--bg);
  color: var(--text);
  line-height: 1.6;
}
a { color: var(--accent); text-decoration-thickness: 1px; text-underline-offset: 3px; }
.site-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 24px;
  padding: 18px clamp(20px, 5vw, 56px);
  border-bottom: 1px solid var(--line);
  background: var(--panel);
  position: sticky;
  top: 0;
  z-index: 10;
}
.brand {
  color: var(--text);
  font-weight: 750;
  font-size: 20px;
  text-decoration: none;
}
nav {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  flex-wrap: wrap;
  gap: 14px;
}
nav a {
  color: var(--muted);
  font-size: 14px;
  text-decoration: none;
}
nav a:hover { color: var(--text); }
.content {
  width: min(920px, calc(100% - 40px));
  margin: 42px auto 56px;
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: clamp(24px, 5vw, 52px);
}
h1, h2, h3, h4 { line-height: 1.2; margin: 1.6em 0 0.55em; }
h1 { margin-top: 0; font-size: clamp(32px, 5vw, 48px); }
h2 { font-size: 26px; border-top: 1px solid var(--line); padding-top: 28px; }
h3 { font-size: 21px; }
p, ul, table, blockquote { margin: 0 0 18px; }
ul { padding-left: 24px; }
code {
  background: #eef2f8;
  border: 1px solid #dfe6f1;
  border-radius: 4px;
  padding: 0.1em 0.35em;
  font-size: 0.92em;
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
th { background: #f1f4f9; }
blockquote {
  border-left: 4px solid var(--accent);
  padding: 12px 18px;
  background: #f3f7ff;
  color: #24324a;
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
@media (max-width: 700px) {
  .site-header { align-items: flex-start; flex-direction: column; }
  nav { justify-content: flex-start; }
  .content { margin-top: 24px; }
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
