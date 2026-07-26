const SECURITY_HEADERS = {
  "content-security-policy":
    "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: https:; script-src 'self' https://cdn.jsdelivr.net; connect-src 'self'; font-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'",
  "referrer-policy": "strict-origin-when-cross-origin",
  "x-content-type-options": "nosniff",
};

function withHeaders(response) {
  const headers = new Headers(response.headers);
  for (const [key, value] of Object.entries(SECURITY_HEADERS)) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

function jsonResponse(body, status = 200) {
  return withHeaders(new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  }));
}

function textEncoder() {
  return new TextEncoder();
}

function textDecoder() {
  return new TextDecoder();
}

function base64urlEncode(bytes) {
  let binary = "";
  for (const byte of new Uint8Array(bytes)) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
}

function base64urlDecode(value) {
  const padded = value.replaceAll("-", "+").replaceAll("_", "/") +
    "=".repeat((4 - (value.length % 4)) % 4);
  const binary = atob(padded);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

function randomBase64url(byteCount) {
  const bytes = crypto.getRandomValues(new Uint8Array(byteCount));
  return base64urlEncode(bytes);
}

async function pkceChallenge(verifier) {
  const digest = await crypto.subtle.digest("SHA-256", textEncoder().encode(verifier));
  return base64urlEncode(digest);
}

async function hmacKey(secret) {
  return crypto.subtle.importKey(
    "raw",
    textEncoder().encode(secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign", "verify"],
  );
}

async function aesKey(secret) {
  const digest = await crypto.subtle.digest("SHA-256", textEncoder().encode(secret));
  return crypto.subtle.importKey("raw", digest, { name: "AES-GCM" }, false, [
    "encrypt",
    "decrypt",
  ]);
}

async function signPayload(payload, secret) {
  const body = base64urlEncode(textEncoder().encode(JSON.stringify(payload)));
  const signature = await crypto.subtle.sign(
    "HMAC",
    await hmacKey(secret),
    textEncoder().encode(body),
  );
  return `${body}.${base64urlEncode(signature)}`;
}

async function signJwt(header, payload, secret) {
  const encodedHeader = base64urlEncode(textEncoder().encode(JSON.stringify(header)));
  const encodedPayload = base64urlEncode(textEncoder().encode(JSON.stringify(payload)));
  const signingInput = `${encodedHeader}.${encodedPayload}`;
  const signature = await crypto.subtle.sign(
    "HMAC",
    await hmacKey(secret),
    textEncoder().encode(signingInput),
  );
  return `${signingInput}.${base64urlEncode(signature)}`;
}

async function verifySignedPayload(token, secret) {
  const [body, signature] = token.split(".");
  if (!body || !signature) {
    throw new Error("Malformed state");
  }
  const ok = await crypto.subtle.verify(
    "HMAC",
    await hmacKey(secret),
    base64urlDecode(signature),
    textEncoder().encode(body),
  );
  if (!ok) {
    throw new Error("State signature mismatch");
  }
  const payload = JSON.parse(textDecoder().decode(base64urlDecode(body)));
  if (!payload.exp || Date.now() > payload.exp) {
    throw new Error("State expired");
  }
  return payload;
}

async function encryptPayload(payload, secret) {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ciphertext = await crypto.subtle.encrypt(
    { name: "AES-GCM", iv },
    await aesKey(secret),
    textEncoder().encode(JSON.stringify(payload)),
  );
  return `${base64urlEncode(iv)}.${base64urlEncode(ciphertext)}`;
}

async function decryptPayload(token, secret) {
  const [iv, ciphertext] = token.split(".");
  if (!iv || !ciphertext) {
    throw new Error("Malformed broker token");
  }
  const plaintext = await crypto.subtle.decrypt(
    { name: "AES-GCM", iv: base64urlDecode(iv) },
    await aesKey(secret),
    base64urlDecode(ciphertext),
  );
  const payload = JSON.parse(textDecoder().decode(plaintext));
  if (!payload.exp || Date.now() > payload.exp) {
    throw new Error("Broker token expired");
  }
  return payload;
}

function oauthConfig(env, requestUrl) {
  const redirectUri = env.ZOOM_OAUTH_REDIRECT_URI ||
    `${requestUrl.origin}/oauth/callback`;
  return {
    clientId: env.ZOOM_OAUTH_PUBLIC_CLIENT_ID || env.ZOOM_OAUTH_CLIENT_ID,
    authorizeUrl: env.ZOOM_OAUTH_AUTHORIZE_URL || "https://zoom.us/oauth/authorize",
    redirectUri,
    scopes: env.ZOOM_OAUTH_SCOPES || "user:read:token user:read:user",
    brokerSecret: env.COREVIDEO_OAUTH_BROKER_SECRET,
  };
}

function requireOauthConfig(config) {
  if (!config.clientId || !config.brokerSecret) {
    return "OAuth broker is not configured. Set ZOOM_OAUTH_PUBLIC_CLIENT_ID and COREVIDEO_OAUTH_BROKER_SECRET.";
  }
  return "";
}

function requireMeetingSdkConfig(env) {
  if (!env.ZOOM_MEETING_SDK_CLIENT_ID || !env.ZOOM_MEETING_SDK_CLIENT_SECRET) {
    return "Meeting SDK broker is not configured. Set ZOOM_MEETING_SDK_CLIENT_ID and ZOOM_MEETING_SDK_CLIENT_SECRET.";
  }
  return "";
}

async function mintMeetingSdkJwt(env) {
  const now = Math.floor(Date.now() / 1000);
  const exp = now + 2 * 60 * 60;
  return signJwt(
    { alg: "HS256", typ: "JWT" },
    {
      appKey: env.ZOOM_MEETING_SDK_CLIENT_ID,
      iat: now - 30,
      exp,
      tokenExp: exp,
    },
    env.ZOOM_MEETING_SDK_CLIENT_SECRET,
  );
}

async function exchangeZoomToken(config, body) {
  const tokenBody = new URLSearchParams(body);
  tokenBody.set("client_id", config.clientId);
  const headers = {
    "content-type": "application/x-www-form-urlencoded",
    "accept": "application/json",
  };

  const response = await fetch("https://zoom.us/oauth/token", {
    method: "POST",
    headers,
    body: tokenBody,
  });
  const text = await response.text();
  if (!response.ok) {
    console.warn("Zoom OAuth token exchange failed", {
      status: response.status,
      client_id_tail: config.clientId.slice(-4),
      redirect_uri: body.redirect_uri,
      body: text.slice(0, 512),
    });
    return { ok: false, status: response.status, text };
  }
  return { ok: true, status: response.status, data: JSON.parse(text) };
}

function callbackPage(returnUrl) {
  const escaped = returnUrl.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;");
  return withHeaders(new Response(`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Return to CoreVideo</title>
  <style>
    body { font-family: system-ui, sans-serif; margin: 0; min-height: 100vh; display: grid; place-items: center; background: #090b12; color: #f7f8ff; }
    main { width: min(520px, calc(100vw - 32px)); }
    a { display: inline-block; margin-top: 20px; padding: 12px 16px; border-radius: 8px; background: #2f7df6; color: white; text-decoration: none; font-weight: 700; }
    p { color: #b9c0d4; line-height: 1.5; }
  </style>
</head>
<body>
  <main>
    <h1>Zoom authorization complete</h1>
    <p>Return to OBS to finish signing CoreVideo into Zoom. If OBS does not open automatically, use the button below.</p>
    <a href="${escaped}">Return to OBS</a>
  </main>
  <meta http-equiv="refresh" content="0; url=${escaped}">
</body>
</html>`, {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "no-store",
      "content-security-policy": "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'",
    },
  }));
}

function zoomFailureMessage(prefix, exchanged) {
  let zoomError = exchanged.text;
  try {
    const parsed = JSON.parse(exchanged.text);
    zoomError = parsed.reason || parsed.message || parsed.error || exchanged.text;
  } catch {
    // Keep the raw text from Zoom when it is not JSON.
  }
  return {
    error: `${prefix}: ${zoomError}`,
    zoom_status: exchanged.status,
  };
}

async function handleOauthStart(request, env) {
  const requestUrl = new URL(request.url);
  const config = oauthConfig(env, requestUrl);
  const configError = requireOauthConfig(config);
  if (configError) {
    return jsonResponse({ error: configError }, 500);
  }

  const localState = requestUrl.searchParams.get("state");
  const returnUri = requestUrl.searchParams.get("return_uri") || "corevideo://oauth/callback";
  if (!localState) {
    return jsonResponse({ error: "Missing CoreVideo OAuth state." }, 400);
  }
  if (returnUri !== "corevideo://oauth/callback") {
    return jsonResponse({ error: "Unsupported OAuth return URI." }, 400);
  }

  const verifier = randomBase64url(64);
  const state = await encryptPayload({
    exp: Date.now() + 10 * 60 * 1000,
    local_state: localState,
    return_uri: returnUri,
    code_verifier: verifier,
  }, config.brokerSecret);

  const zoomUrl = new URL(config.authorizeUrl);
  zoomUrl.searchParams.set("response_type", "code");
  zoomUrl.searchParams.set("client_id", config.clientId);
  zoomUrl.searchParams.set("redirect_uri", config.redirectUri);
  zoomUrl.searchParams.set("state", state);
  zoomUrl.searchParams.set("code_challenge", await pkceChallenge(verifier));
  zoomUrl.searchParams.set("code_challenge_method", "S256");
  if (config.scopes) {
    zoomUrl.searchParams.set("scope", config.scopes);
  }
  return Response.redirect(zoomUrl.toString(), 302);
}

async function handleOauthCallback(request, env) {
  const requestUrl = new URL(request.url);
  const config = oauthConfig(env, requestUrl);
  const configError = requireOauthConfig(config);
  if (configError) {
    return jsonResponse({ error: configError }, 500);
  }

  const zoomError = requestUrl.searchParams.get("error");
  const stateToken = requestUrl.searchParams.get("state");
  if (!stateToken) {
    return jsonResponse({ error: "Missing OAuth state." }, 400);
  }

  let state;
  try {
    state = await decryptPayload(stateToken, config.brokerSecret);
  } catch (error) {
    return jsonResponse({ error: error.message }, 400);
  }

  const returnUrl = new URL(state.return_uri);
  returnUrl.searchParams.set("state", state.local_state);
  if (zoomError) {
    returnUrl.searchParams.set("error", zoomError);
    return callbackPage(returnUrl.toString());
  }

  const code = requestUrl.searchParams.get("code");
  if (!code) {
    return jsonResponse({ error: "Missing OAuth authorization code." }, 400);
  }

  const brokerToken = await encryptPayload({
    exp: Date.now() + 5 * 60 * 1000,
    code,
    code_verifier: state.code_verifier,
  }, config.brokerSecret);
  returnUrl.searchParams.set("broker_token", brokerToken);
  return callbackPage(returnUrl.toString());
}

async function readJson(request) {
  try {
    return await request.json();
  } catch {
    return {};
  }
}

async function handleOauthRedeem(request, env) {
  if (request.method !== "POST") {
    return jsonResponse({ error: "Use POST." }, 405);
  }
  const config = oauthConfig(env, new URL(request.url));
  const configError = requireOauthConfig(config);
  if (configError) {
    return jsonResponse({ error: configError }, 500);
  }
  const body = await readJson(request);
  if (!body.broker_token) {
    return jsonResponse({ error: "Missing broker token." }, 400);
  }
  try {
    const payload = await decryptPayload(body.broker_token, config.brokerSecret);
    if (!payload.code) {
      return jsonResponse({ error: "Broker token did not include an authorization code." }, 400);
    }
    if (!payload.code_verifier) {
      return jsonResponse({ error: "Broker token did not include a PKCE verifier." }, 400);
    }
    const exchanged = await exchangeZoomToken(config, {
      grant_type: "authorization_code",
      code: payload.code,
      redirect_uri: config.redirectUri,
      code_verifier: payload.code_verifier,
    });
    if (!exchanged.ok) {
      return jsonResponse(zoomFailureMessage("Zoom token exchange failed",
                                             exchanged), 502);
    }
    return jsonResponse(exchanged.data);
  } catch (error) {
    return jsonResponse({ error: error.message }, 400);
  }
}

async function handleOauthRefresh(request, env) {
  if (request.method !== "POST") {
    return jsonResponse({ error: "Use POST." }, 405);
  }
  const config = oauthConfig(env, new URL(request.url));
  const configError = requireOauthConfig(config);
  if (configError) {
    return jsonResponse({ error: configError }, 500);
  }
  const body = await readJson(request);
  if (!body.refresh_token) {
    return jsonResponse({ error: "Missing refresh token." }, 400);
  }
  const refreshed = await exchangeZoomToken(config, {
    grant_type: "refresh_token",
    refresh_token: body.refresh_token,
  });
  if (!refreshed.ok) {
    return jsonResponse({
      error: "Zoom token refresh failed.",
      zoom_status: refreshed.status,
      zoom_response: refreshed.text,
    }, 502);
  }
  return jsonResponse(refreshed.data);
}

async function handleMeetingSdkJwt(request, env) {
  if (request.method !== "POST") {
    return jsonResponse({ error: "Use POST." }, 405);
  }
  const configError = requireMeetingSdkConfig(env);
  if (configError) {
    return jsonResponse({ error: configError }, 500);
  }
  const body = await readJson(request);
  if (!body.access_token) {
    return jsonResponse({ error: "Missing Zoom OAuth access token." }, 400);
  }

  const userResponse = await fetch("https://api.zoom.us/v2/users/me", {
    headers: {
      "authorization": `Bearer ${body.access_token}`,
      "accept": "application/json",
    },
  });
  if (!userResponse.ok) {
    return jsonResponse({
      error: "Zoom OAuth access token could not be validated.",
      zoom_status: userResponse.status,
      zoom_response: (await userResponse.text()).slice(0, 512),
    }, 401);
  }

  return jsonResponse({
    sdk_jwt: await mintMeetingSdkJwt(env),
    expires_in: 7200,
  });
}

async function fetchAsset(request, env, pathname) {
  const url = new URL(request.url);
  url.pathname = pathname;
  return env.ASSETS.fetch(new Request(url, request));
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // corevideo.io is the primary domain. Redirect the www subdomain to the
    // apex so there is a single canonical host, preserving path and query.
    if (url.hostname === "www.corevideo.io") {
      url.hostname = "corevideo.io";
      return Response.redirect(url.toString(), 301);
    }

    const pathname = url.pathname.replace(/\/$/u, "") || "/";

    // Download CTA: always send users to the latest published release, where the
    // signed Windows installer (CoreVideo-Setup-*.exe) and checksum are listed.
    if (pathname === "/download") {
      return Response.redirect(
        "https://github.com/iamfatness/CoreVideo/releases/latest",
        302,
      );
    }
    if (pathname === "/oauth/start") {
      return handleOauthStart(request, env);
    }
    if (pathname === "/oauth/callback") {
      return handleOauthCallback(request, env);
    }
    if (pathname === "/oauth/redeem") {
      return handleOauthRedeem(request, env);
    }
    if (pathname === "/oauth/refresh") {
      return handleOauthRefresh(request, env);
    }
    if (pathname === "/oauth/sdk-jwt") {
      return handleMeetingSdkJwt(request, env);
    }

    let response = await env.ASSETS.fetch(request);
    if (response.status !== 404) {
      return withHeaders(response);
    }

    if (!url.pathname.endsWith("/") && !url.pathname.includes(".")) {
      url.pathname += "/";
      return Response.redirect(url.toString(), 301);
    }

    if (url.pathname.endsWith("/")) {
      response = await fetchAsset(request, env, `${url.pathname}index.html`);
      if (response.status !== 404) {
        return withHeaders(response);
      }
    }

    response = await fetchAsset(request, env, "/404.html");
    return withHeaders(new Response(response.body, {
      status: 404,
      headers: response.headers,
    }));
  },
};
