#!/usr/bin/env node
// v0.76.10 (real fix, not this pass's own bump -- see roadmap.md for the
// full CORS trace): worker.js's /api/proxy route is what makes v86's
// cors_proxy option actually work for real, server-to-server, never-CORS-
// limited fetches to the three hosts this kernel's own code asks for
// (ip-api.com, a.tile.opentopomap.org, mt0.google.com). Deliberately
// NOT a general-purpose open proxy: isAllowedTarget() is a strict
// allowlist, checked here directly.
//
// This container's own outbound network is itself behind a host-level
// allowlist (see roadmap.md's v0.76.7 entry), so a real end-to-end fetch
// to any of the three allowed hosts isn't reproducible from here either
// -- these checks cover the real, deterministic logic (allowlist
// rejection, missing/invalid url handling, and the success path's
// header handling with a mocked fetch), the same honest scoping
// rtc-timezone-check.mjs already uses for the CORS/RTC fix above it.
import { isAllowedTarget, handleProxy, ALLOWED_HOSTS } from "../../worker.js";

let failures = 0;
function check(label, cond) {
  if (!cond) { console.log(`FAIL: ${label}`); failures++; }
  else console.log(`ok: ${label}`);
}

for (const host of ALLOWED_HOSTS) {
  check(`allows the real host ${host}`, isAllowedTarget(new URL(`http://${host}/`)));
}
check("allows Open-Meteo, the forecast host weather_fetch asks for (its absence left the demo's Weather empty)", isAllowedTarget(new URL("https://api.open-meteo.com/v1/forecast?latitude=49.1&longitude=-122.6&current=temperature_2m,weather_code")));
check("rejects an arbitrary host", !isAllowedTarget(new URL("http://evil.example.com/")));
check("rejects localhost (no SSRF to the Worker's own network)", !isAllowedTarget(new URL("http://localhost/")));
check("rejects a private IP", !isAllowedTarget(new URL("http://169.254.169.254/")));
check("rejects a non-http(s) protocol", !isAllowedTarget(new URL("file:///etc/passwd")));
check("rejects a lookalike host (suffix match would be a real bypass)", !isAllowedTarget(new URL("http://evil-ip-api.com/")));

const missingUrlResp = await handleProxy(new Request("https://joshuatree.heyitsmejosh.com/api/proxy"));
check("missing url param -> 400", missingUrlResp.status === 400);

const invalidUrlResp = await handleProxy(new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=not-a-url"));
check("invalid url param -> 400", invalidUrlResp.status === 400);

{
  const realFetch = globalThis.fetch;
  let fetchCalled = false;
  globalThis.fetch = async () => { fetchCalled = true; return new Response("should never be reached"); };
  try {
    const disallowedResp = await handleProxy(new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent("http://evil.example.com/steal")));
    check("disallowed host -> 403", disallowedResp.status === 403);
    check("disallowed host never reaches the real fetch call", !fetchCalled);
  } finally {
    globalThis.fetch = realFetch;
  }
}

// Success path: real fetch is mocked so this runs with no real network at
// all, deterministic in any environment. Confirms the exact real target
// URL is what gets fetched (not something else), and that the response
// carries a real Access-Control-Allow-Origin header while set-cookie
// (an upstream host has no business setting cookies through a proxy
// acting on this page's behalf) is stripped.
const realFetch = globalThis.fetch;
let fetchedUrl = null;
globalThis.fetch = async (url, init) => {
  fetchedUrl = url;
  return new Response("tile bytes", {
    status: 200,
    headers: { "content-type": "image/png", "set-cookie": "should-not-survive=1" },
  });
};
try {
  const target = "http://a.tile.opentopomap.org/14/1234/5678.png";
  const okResp = await handleProxy(new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent(target)));
  check("allowed host -> 200", okResp.status === 200);
  check("fetched exactly the requested target URL", fetchedUrl === target);
  check("response carries Access-Control-Allow-Origin", okResp.headers.get("access-control-allow-origin") === "*");
  check("set-cookie stripped from the proxied response", okResp.headers.get("set-cookie") === null);
  check("upstream body passed through", (await okResp.text()) === "tile bytes");
} finally {
  globalThis.fetch = realFetch;
}

// v0.76.15: real regression guard. libv86.js upgrades a guest's plain-
// HTTP target to https:// whenever the page itself is https (a real,
// deliberate mixed-content fix in v86 -- confirmed by reading its own
// source), so a real browser visitor's geo request reaches this Worker
// as `url=https://ip-api.com/...`, not http, regardless of what the
// kernel's own http_get() call asked for. ip-api.com's free tier 403s
// over HTTPS (confirmed live against production -- the identical
// proxied request returns 200 over HTTP, 403 over HTTPS, nothing else
// differs), so this Worker must force ip-api.com's own scheme back to
// http before fetching, or every real visitor's geo lookup silently
// fails forever (and with it, kernel.c's own geo_have-gated wall_fetch()
// auto-satellite-load never fires -- the actual final root cause behind
// "stale wallpaper" surviving the CORS-proxy fix and the Settings-click
// regression fix, both real, both necessary, neither sufficient alone).
{
  const realFetch = globalThis.fetch;
  let fetchedUrl = null;
  globalThis.fetch = async (url) => { fetchedUrl = url; return new Response("{}", { status: 200 }); };
  try {
    const httpsTarget = "https://ip-api.com/json/";
    await handleProxy(new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent(httpsTarget)));
    check("ip-api.com forced back to http:// even when the browser relayed https://", fetchedUrl === "http://ip-api.com/json/");
  } finally {
    globalThis.fetch = realFetch;
  }
}
// The other two allowed hosts are untouched -- real map/tile services,
// no known scheme restriction, so forcing a scheme there would be an
// unjustified special case, not a fix for a real, confirmed problem.
{
  const realFetch = globalThis.fetch;
  let fetchedUrl = null;
  globalThis.fetch = async (url) => { fetchedUrl = url; return new Response("tile", { status: 200 }); };
  try {
    const httpsTarget = "https://a.tile.opentopomap.org/14/1234/5678.png";
    await handleProxy(new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent(httpsTarget)));
    check("a.tile.opentopomap.org's own scheme left untouched", fetchedUrl === httpsTarget);
  } finally {
    globalThis.fetch = realFetch;
  }
}

// 1.0.12: Chat's Samantha route (kernel/chat.h's chat_send -> Turing's own
// /api/chat). Deliberately NOT part of ALLOWED_HOSTS/isAllowedTarget --
// this is a separate, tight exception checked directly, since it is the
// one and only route that ever forwards a guest-supplied method/body
// anywhere.
{
  const realFetch = globalThis.fetch;
  let fetchedUrl = null, fetchedInit = null;
  globalThis.fetch = async (url, init) => { fetchedUrl = url; fetchedInit = init; return new Response(
    JSON.stringify({ model: "samantha", message: { role: "assistant", content: "hi" }, done: true }),
    { status: 200, headers: { "content-type": "application/json" } });
  };
  try {
    const target = "https://turing.heyitsmejosh.com/api/chat";
    const body = JSON.stringify({ model: "samantha", stream: false, think: false, messages: [{ role: "user", content: "hello" }] });
    const req = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent(target), {
      method: "POST",
      headers: { "content-type": "application/json" },
      body,
    });
    const resp = await handleProxy(req);
    check("Samantha POST forwarded to the real target URL", fetchedUrl === target);
    check("Samantha POST forwarded with method POST (not downgraded to GET)", fetchedInit && fetchedInit.method === "POST");
    check("Samantha POST forwarded with the real request body", fetchedInit && new TextDecoder().decode(fetchedInit.body) === body);
    check("Samantha POST forwarded with a JSON content-type", fetchedInit && (fetchedInit.headers["Content-Type"] || fetchedInit.headers["content-type"]) === "application/json");
    check("Samantha POST reply passed straight through, status 200", resp.status === 200);
    check("Samantha POST reply carries Access-Control-Allow-Origin", resp.headers.get("access-control-allow-origin") === "*");
    const replyBody = await resp.json();
    check("Samantha POST reply body reached the guest unmodified", replyBody.message && replyBody.message.content === "hi");
  } finally {
    globalThis.fetch = realFetch;
  }
}

// A POST to any OTHER allowed host must still be downgraded to a bare GET
// with no body -- the Samantha exception must not have loosened the
// existing GET-only contract for every real target this kernel already
// asks for (geo_fetch/weather_fetch/wall_fetch never POST, so there is
// nothing legitimate this could ever forward).
{
  const realFetch = globalThis.fetch;
  let fetchedInit = null;
  globalThis.fetch = async (url, init) => { fetchedInit = init; return new Response("tile", { status: 200 }); };
  try {
    const target = "http://a.tile.opentopomap.org/14/1234/5678.png";
    const req = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent(target), {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: "should never be forwarded",
    });
    await handleProxy(req);
    check("a POST to any other allowed host is still downgraded to GET", fetchedInit && fetchedInit.method === "GET");
    check("...with no body forwarded", fetchedInit && fetchedInit.body === undefined);
  } finally {
    globalThis.fetch = realFetch;
  }
}

// An oversized body is refused before ever reaching fetch.
{
  const realFetch = globalThis.fetch;
  let fetchCalled = false;
  globalThis.fetch = async () => { fetchCalled = true; return new Response("should never be reached"); };
  try {
    const target = "https://turing.heyitsmejosh.com/api/chat";
    const oversized = "x".repeat(8193);
    const req = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent(target), {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: oversized,
    });
    const resp = await handleProxy(req);
    check("an oversized Samantha body is refused (413)", resp.status === 413);
    check("an oversized body never reaches the real fetch call", !fetchCalled);
  } finally {
    globalThis.fetch = realFetch;
  }
}

// The Origin/host checks still hold: the exception is scoped to exactly
// this host+path+method+content-type, nothing wider slipped in with it.
{
  const realFetch = globalThis.fetch;
  let fetchCalled = false;
  globalThis.fetch = async () => { fetchCalled = true; return new Response("should never be reached"); };
  try {
    // A GET to the same host+path is not the exception (wrong method) and
    // turing.heyitsmejosh.com is still not in ALLOWED_HOSTS, so it 403s
    // exactly like any other unlisted host, same as before this pass.
    const getReq = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent("https://turing.heyitsmejosh.com/api/chat"));
    const getResp = await handleProxy(getReq);
    check("a bare GET to turing.heyitsmejosh.com/api/chat still 403s (not part of the allowlist)", getResp.status === 403);

    // A POST to a different path on the same host is not the exception either.
    const wrongPathReq = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent("https://turing.heyitsmejosh.com/api/other"), {
      method: "POST", headers: { "content-type": "application/json" }, body: "{}",
    });
    const wrongPathResp = await handleProxy(wrongPathReq);
    check("a POST to a different path on turing.heyitsmejosh.com still 403s", wrongPathResp.status === 403);

    // Wrong content-type on the real chat path is rejected, not silently forwarded.
    const wrongTypeReq = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url=" + encodeURIComponent("https://turing.heyitsmejosh.com/api/chat"), {
      method: "POST", headers: { "content-type": "text/plain" }, body: "{}",
    });
    const wrongTypeResp = await handleProxy(wrongTypeReq);
    check("a non-JSON content-type on the Samantha route is rejected (415)", wrongTypeResp.status === 415);

    check("none of the above reached the real fetch call", !fetchCalled);
  } finally {
    globalThis.fetch = realFetch;
  }
}
check("turing.heyitsmejosh.com is deliberately NOT in the general allowlist (the exception is narrow, checked separately)", !isAllowedTarget(new URL("https://turing.heyitsmejosh.com/api/chat")));

if (failures > 0) {
  console.log(`FAIL: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("PASS: /api/proxy allowlists exactly the real hosts this kernel uses, rejects everything else, handles the success path correctly, and forwards Samantha's POST /api/chat body only for that exact host/path/content-type/size");
