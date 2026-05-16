const ORIGIN = "https://raw.githubusercontent.com/iamfatness/CoreVideo/4d0a776/public";
const VERSION = "4d0a776";

const aliases = new Map([
  ["/terms-of-use", "/terms/"],
  ["/Terms-of-Use", "/terms/"],
  ["/privacy-policy", "/privacy/"],
  ["/Privacy-Policy", "/privacy/"],
  ["/Support", "/support/"],
  ["/docs", "/documentation/"],
]);

const htmlPaths = new Set(["/", "/terms/", "/privacy/", "/support/", "/documentation/", "/oauth/"]);

function normalizePath(pathname) {
  if (aliases.has(pathname))
    return { redirect: aliases.get(pathname) };
  if (pathname === "/terms")
    return { redirect: "/terms/" };
  if (pathname === "/privacy")
    return { redirect: "/privacy/" };
  if (pathname === "/support")
    return { redirect: "/support/" };
  if (pathname === "/documentation")
    return { redirect: "/documentation/" };
  if (pathname === "/oauth")
    return { redirect: "/oauth/" };
  if (pathname === "/assets/site.css")
    return { asset: "/assets/site.css", type: "text/css; charset=utf-8" };
  if (pathname === "/assets/corevideo-logo.jpg")
    return { asset: "/assets/corevideo-logo.jpg", type: "image/jpeg" };
  if (htmlPaths.has(pathname))
    return { asset: pathname + "index.html", type: "text/html; charset=utf-8" };
  return { asset: "/index.html", type: "text/html; charset=utf-8", status: 404 };
}

async function fetchAsset(asset) {
  const response = await fetch(`${ORIGIN}${asset}?v=${VERSION}`, {
    cf: { cacheTtl: 0, cacheEverything: false },
  });
  if (!response.ok)
    return new Response("Not found", { status: 404 });
  return response;
}

function siteHeaders(type) {
  return new Headers({
    "content-type": type,
    "cache-control": "no-cache",
    "x-content-type-options": "nosniff",
    "referrer-policy": "strict-origin-when-cross-origin",
    "content-security-policy":
      "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: https:; script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; connect-src 'self'; frame-ancestors 'none'",
  });
}

addEventListener("fetch", (event) => {
  event.respondWith((async () => {
    const url = new URL(event.request.url);
    const resolved = normalizePath(url.pathname);
    if (resolved.redirect)
      return Response.redirect(url.origin + resolved.redirect, 301);

    const upstream = await fetchAsset(resolved.asset);
    return new Response(upstream.body, {
      status: resolved.status || upstream.status,
      headers: siteHeaders(resolved.type),
    });
  })());
});
