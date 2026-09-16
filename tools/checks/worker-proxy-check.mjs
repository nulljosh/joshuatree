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

if (failures > 0) {
  console.log(`FAIL: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("PASS: /api/proxy allowlists exactly the three real hosts this kernel uses, rejects everything else, and handles the success path correctly");
