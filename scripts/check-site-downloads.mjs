// Asserts that every pinned GitHub release download on the generated site
// actually exists. This fails silently in production: the button renders, and
// the visitor who clicks it gets a 404. That shipped for weeks -- the macOS
// button pointed at CoreVideo-Setup-v0.1.45-beta.3.pkg, an asset that was never
// uploaded (reported 2026-10-03). Links to releases/latest are not checked;
// GitHub resolves those itself. Run after scripts/build-site.mjs.
//
//   node scripts/build-site.mjs && node scripts/check-site-downloads.mjs

import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const outDir = path.join(root, "public");
const DOWNLOAD_PREFIX = "https://github.com/iamfatness/CoreVideo/releases/download/";

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

const urls = new Map(); // url -> first page that links it
for (const file of findHtmlFiles(outDir)) {
  const html = fs.readFileSync(file, "utf8");
  for (const match of html.matchAll(/href="([^"]+)"/g)) {
    const url = match[1];
    if (url.startsWith(DOWNLOAD_PREFIX) && !urls.has(url))
      urls.set(url, path.relative(outDir, file));
  }
}

// One byte is enough to prove the asset exists; several of these are hundreds
// of MB. GitHub redirects to its asset host, which honours Range.
async function status(url) {
  try {
    const res = await fetch(url, { headers: { Range: "bytes=0-0" }, redirect: "follow" });
    await res.body?.cancel();
    return res.status;
  } catch (err) {
    return `network error: ${err.message}`;
  }
}

const failures = [];
for (const [url, page] of urls) {
  const code = await status(url);
  const ok = code === 200 || code === 206;
  console.log(`${ok ? "ok  " : "FAIL"} ${code} ${url}`);
  if (!ok) failures.push(`${url} (linked from ${page}) returned ${code}`);
}

if (failures.length) {
  console.error(`\n${failures.length} dead download link(s):`);
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log(`All ${urls.size} pinned download link(s) resolve.`);
