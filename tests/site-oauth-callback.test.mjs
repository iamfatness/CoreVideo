import assert from "node:assert/strict";
import { test } from "node:test";
import worker from "../site-worker.js";

const origin = "https://corevideo.iamfatness.us";
const env = {
  ZOOM_OAUTH_PUBLIC_CLIENT_ID: "test-client",
  COREVIDEO_OAUTH_BROKER_SECRET: "test-broker-key",
};

function start(returnUri) {
  const url = new URL("/oauth/start", origin);
  url.searchParams.set("state", "test-local-state");
  if (returnUri) url.searchParams.set("return_uri", returnUri);
  return worker.fetch(new Request(url), env);
}

for (const [name, uri, expected] of [
  ["OBS", "corevideo-obs://oauth/callback", "corevideo-obs://oauth/callback"],
  ["Pro and older OBS", "corevideo://oauth/callback", "corevideo://oauth/callback"],
  ["omitted return URI", null, "corevideo://oauth/callback"],
  ["ZComms", "http://127.0.0.1:49152/oauth/callback", "http://127.0.0.1:49152/oauth/callback"],
]) {
  test(`OAuth returns to ${name}`, async () => {
    const response = await start(uri);
    assert.equal(response.status, 302);
    const authorize = new URL(response.headers.get("location"));
    assert.equal(authorize.searchParams.get("redirect_uri"), `${origin}/oauth/callback`);
    const callback = new URL("/oauth/callback", origin);
    callback.searchParams.set("state", authorize.searchParams.get("state"));
    callback.searchParams.set("code", "test-zoom-code");
    const page = await worker.fetch(new Request(callback), env);
    assert.equal(page.status, 200);
    assert.ok((await page.text()).includes(`href="${expected}?state=test-local-state&amp;broker_token=`));
  });
}

test("OAuth rejects an unlisted OBS callback", async () => {
  const response = await start("corevideo-obs://oauth/other");
  assert.equal(response.status, 400);
});
