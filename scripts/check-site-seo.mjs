// Asserts the SEO invariants of the generated site. These are all things that
// fail silently in production: a page ships, looks fine, and simply never
// ranks or renders a link preview. Run after scripts/build-site.mjs.
//
//   node scripts/build-site.mjs && node scripts/check-site-seo.mjs

import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const outDir = path.join(root, "public");
const PRIMARY_ORIGIN = "https://corevideo.io";

const failures = [];
function fail(message) {
  failures.push(message);
}

if (!fs.existsSync(outDir)) {
  console.error("public/ does not exist - run scripts/build-site.mjs first.");
  process.exit(1);
}

function findHtmlFiles(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) return findHtmlFiles(full);
    return entry.name.endsWith(".html") ? [full] : [];
  });
}

function attr(html, pattern) {
  return html.match(pattern)?.[1] ?? null;
}

const REQUIRED_SOCIAL = [
  ["og:title", /<meta property="og:title" content="([^"]*)"/],
  ["og:description", /<meta property="og:description" content="([^"]*)"/],
  ["og:image", /<meta property="og:image" content="([^"]*)"/],
  ["og:url", /<meta property="og:url" content="([^"]*)"/],
  ["og:type", /<meta property="og:type" content="([^"]*)"/],
  ["twitter:card", /<meta name="twitter:card" content="([^"]*)"/],
  ["twitter:image", /<meta name="twitter:image" content="([^"]*)"/],
];

const htmlFiles = findHtmlFiles(outDir);
if (htmlFiles.length === 0) fail("no HTML files found in public/");

// canonical URL -> the titles seen on pages claiming it. Aliases legitimately
// duplicate a canonical page, so uniqueness is checked per canonical, not per
// file.
const titlesByCanonical = new Map();

for (const file of htmlFiles) {
  const rel = path.relative(outDir, file).replaceAll("\\", "/");
  const html = fs.readFileSync(file, "utf8");

  const title = attr(html, /<title>([^<]*)<\/title>/);
  const description = attr(html, /<meta name="description" content="([^"]*)"/);
  const canonical = attr(html, /<link rel="canonical" href="([^"]*)"/);

  if (!title) fail(`${rel}: missing <title>`);
  if (title && /^(.*) \| \1$/.test(title)) {
    fail(`${rel}: title repeats the site name ("${title}")`);
  }
  if (title && title.length > 65) {
    fail(`${rel}: title is ${title.length} chars, over the ~65 char SERP limit ("${title}")`);
  }
  if (!description) fail(`${rel}: missing meta description`);
  if (description && (description.length < 50 || description.length > 165)) {
    fail(`${rel}: meta description is ${description.length} chars, outside 50-165`);
  }

  if (!canonical) {
    fail(`${rel}: missing rel=canonical`);
  } else {
    if (!canonical.startsWith(PRIMARY_ORIGIN)) {
      fail(`${rel}: canonical "${canonical}" does not point at ${PRIMARY_ORIGIN}`);
    }
    if (!titlesByCanonical.has(canonical)) titlesByCanonical.set(canonical, new Set());
    titlesByCanonical.get(canonical).add(title);
  }

  for (const [name, pattern] of REQUIRED_SOCIAL) {
    if (!pattern.test(html)) fail(`${rel}: missing ${name}`);
  }

  const ogUrl = attr(html, /<meta property="og:url" content="([^"]*)"/);
  if (canonical && ogUrl && canonical !== ogUrl) {
    fail(`${rel}: og:url "${ogUrl}" disagrees with canonical "${canonical}"`);
  }
}

// Two different pages sharing a title means they compete for the same query.
const seenTitles = new Map();
for (const [canonical, titles] of titlesByCanonical) {
  if (titles.size > 1) {
    fail(`${canonical}: served with conflicting titles ${[...titles].map((t) => `"${t}"`).join(", ")}`);
  }
  const title = [...titles][0];
  if (seenTitles.has(title)) {
    fail(`duplicate title "${title}" on ${seenTitles.get(title)} and ${canonical}`);
  }
  seenTitles.set(title, canonical);
}

// Sitemap must be well-formed enough to parse, and every URL it advertises has
// to correspond to a page that actually got built.
const sitemapPath = path.join(outDir, "sitemap.xml");
if (!fs.existsSync(sitemapPath)) {
  fail("sitemap.xml was not generated");
} else {
  const sitemap = fs.readFileSync(sitemapPath, "utf8");
  const locs = [...sitemap.matchAll(/<loc>([^<]+)<\/loc>/g)].map((m) => m[1]);
  if (locs.length === 0) fail("sitemap.xml lists no URLs");

  for (const loc of locs) {
    const relPath = loc.replace(PRIMARY_ORIGIN, "").replace(/^\//, "");
    const target = path.join(outDir, relPath, "index.html");
    if (!fs.existsSync(target)) {
      fail(`sitemap lists ${loc} but ${path.relative(outDir, target)} was not built`);
    }
  }

  // Every canonical page should be discoverable from the sitemap.
  for (const canonical of titlesByCanonical.keys()) {
    if (!locs.includes(canonical)) fail(`${canonical} is canonical but missing from sitemap.xml`);
  }
}

const robotsPath = path.join(outDir, "robots.txt");
if (!fs.existsSync(robotsPath)) {
  fail("robots.txt was not generated");
} else if (!fs.readFileSync(robotsPath, "utf8").includes(`Sitemap: ${PRIMARY_ORIGIN}/sitemap.xml`)) {
  fail("robots.txt does not advertise the sitemap");
}

// The declared og:image dimensions have to match the actual file, or the card
// renders cropped (or not at all) on the networks that pre-validate it.
function jpegSize(buffer) {
  let offset = 2; // skip SOI
  while (offset < buffer.length) {
    if (buffer[offset] !== 0xff) return null;
    const marker = buffer[offset + 1];
    const length = buffer.readUInt16BE(offset + 2);
    // SOF0/1/2/3, 9/10/11, 13/14/15 all carry the frame dimensions.
    if (marker >= 0xc0 && marker <= 0xcf && ![0xc4, 0xc8, 0xcc].includes(marker)) {
      return { height: buffer.readUInt16BE(offset + 5), width: buffer.readUInt16BE(offset + 7) };
    }
    offset += 2 + length;
  }
  return null;
}

const shareImage = path.join(outDir, "assets", "corevideo-share.jpg");
if (!fs.existsSync(shareImage)) {
  fail("assets/corevideo-share.jpg (the og:image) was not copied into public/");
} else {
  const size = jpegSize(fs.readFileSync(shareImage));
  if (!size) {
    fail("could not read the dimensions of assets/corevideo-share.jpg");
  } else if (size.width !== 1200 || size.height !== 630) {
    fail(`og:image is ${size.width}x${size.height}, but the tags declare 1200x630`);
  }
}

if (failures.length) {
  console.error(`SEO check failed (${failures.length} problem${failures.length === 1 ? "" : "s"}):`);
  for (const failure of failures) console.error(`  - ${failure}`);
  process.exit(1);
}

console.log(`SEO check passed: ${htmlFiles.length} pages, ${titlesByCanonical.size} canonical URLs.`);
