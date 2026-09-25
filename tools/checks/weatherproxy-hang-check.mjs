#!/usr/bin/env node
// Regression check for the live-site bug Joshua reported: "Chat works for
// a question or two, then FREEZES asking about the weather. The whole OS
// stops responding." Root cause, found by reading worker.js directly: its
// handleProxy() forwarded every request (the Samantha chat/pick POST
// branch, and the plain GET branch geo_fetch/weather_fetch/wall_fetch all
// share) through a bare, un-timed-out fetch(). v86's cors_proxy network
// mode turns the guest kernel's one TCP connection into exactly one
// browser-side fetch() through this Worker and buffers the WHOLE reply
// before ever handing a byte back to the guest's virtual NIC -- so the
// guest's own bounded, ticks()-driven timeouts (drivers/net.c's
// tcp_get_timeout, kernel.c's WX_REPLY_TIMEOUT_TICKS) never get a chance
// to fire: nothing ever arrives for them to time out against. A real
// upstream that accepts the connection and then never answers (ordinary,
// not contrived -- a slow or half-dead host is enough) left that fetch()
// pending for Cloudflare's own subrequest ceiling, which reads to a real
// visitor as the whole desktop freezing.
//
// This proves the fix at the one shared layer every proxied call goes
// through (handleProxy itself, not just the weather-specific caller):
// with a mocked global.fetch that NEVER resolves (the exact "connected
// but silent forever" shape worker.js's own timeout comment now
// documents), handleProxy must still return -- within a real wall-clock
// bound well under a hung test -- rather than hanging the test (and, in
// production, the guest) forever. Run against the pre-fix worker.js this
// hangs past DEADLINE_MS and fails; against the fix it returns promptly
// with a real 504.
//
// Same honest scoping as worker-proxy-check.mjs's own header comment:
// no live network, no real Cloudflare Worker, just the real allowlist/
// fetch-wrapping logic under Node with global.fetch mocked.
import { handleProxy } from "../../worker.js";

let failures = 0;
function check(label, cond) {
  if (!cond) { console.log(`FAIL: ${label}`); failures++; }
  else console.log(`ok: ${label}`);
}

// Real shape: TCP/HTTP connects fine, upstream just never sends a body.
// A real "hung" fetch() never settles on its own, matching a peer that
// accepted the connection and went silent (the exact case worker.js's own
// comment above fetchWithTimeout calls out) -- but it DOES honor an
// AbortSignal, exactly like the real fetch()/undici this Worker runs on
// in production. That's the one thing this mock must get right: it's
// what lets this check tell "the code passes no bound at all" (hangs
// past DEADLINE_MS below) apart from "the code passes AbortSignal.timeout
// and the fetch call itself correctly rejects when aborted" (returns
// promptly with a real 504).
function neverResolvingFetch(url, init) {
  return new Promise((resolve, reject) => {
    const signal = init && init.signal;
    if (signal) {
      if (signal.aborted) { reject(new DOMException("The operation was aborted.", "AbortError")); return; }
      signal.addEventListener("abort", () => reject(new DOMException("The operation was aborted.", "AbortError")));
    }
    // otherwise: never resolves, never rejects -- a genuinely silent peer
  });
}

const DEADLINE_MS = 15000; // generous: the fix's own bound is 8000ms

async function withDeadline(label, promise) {
  const t0 = Date.now();
  let timedOut = false;
  const guard = new Promise((resolve) => setTimeout(() => { timedOut = true; resolve(undefined); }, DEADLINE_MS));
  const result = await Promise.race([promise, guard]);
  const elapsed = Date.now() - t0;
  check(`${label} returned within ${DEADLINE_MS}ms (took ${elapsed}ms) instead of hanging`, !timedOut);
  return timedOut ? null : result;
}

const realFetch = globalThis.fetch;
globalThis.fetch = neverResolvingFetch;
try {
  // 1) The plain-GET branch: exactly what geo_fetch/weather_fetch/
  //    wall_fetch all funnel through, real reproduction of "the weather
  //    call hangs the desktop" against a silent api.open-meteo.com.
  const weatherReq = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url="
    + encodeURIComponent("https://api.open-meteo.com/v1/forecast?latitude=49.1&longitude=-122.6&current=temperature_2m,weather_code"));
  const weatherResp = await withDeadline("weather (open-meteo GET) proxy call", handleProxy(weatherReq));
  if (weatherResp) check("weather proxy call answers with a real gateway-timeout status, not a fabricated success", weatherResp.status === 504);

  // 2) The Samantha chat/pick POST branch: same shared fetchWithTimeout,
  //    proves the fix covers Chat's own network calls too, not weather
  //    alone -- "the shared function all callers use."
  const chatReq = new Request("https://joshuatree.heyitsmejosh.com/api/proxy?url="
    + encodeURIComponent("https://turing.heyitsmejosh.com/api/chat"), {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ model: "samantha", messages: [{ role: "user", content: "what is the capital of france" }] }),
  });
  const chatResp = await withDeadline("chat (/api/chat POST) proxy call", handleProxy(chatReq));
  if (chatResp) check("chat proxy call answers with a real gateway-timeout status, not a fabricated success", chatResp.status === 504);
} finally {
  globalThis.fetch = realFetch;
}

if (failures) {
  console.log(`FAIL: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("PASS: a silent upstream can no longer hang the proxy (and with it, the guest desktop) past a bounded timeout");
