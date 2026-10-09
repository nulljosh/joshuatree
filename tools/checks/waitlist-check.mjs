#!/usr/bin/env node
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
// Dev-kit waitlist: worker.js's /api/waitlist (POST) and /api/waitlist/count
// (GET). Same idea as worker-proxy-check.mjs -- no real Cloudflare Worker or
// KV namespace here, so this exercises the real handler functions directly
// against a fake KV (a plain Map), which is a faithful enough stand-in for
// KV's put/list contract to prove the real logic: valid email accepted and
// lowercased, invalid email rejected, oversized body rejected, duplicate
// emails overwrite instead of growing the namespace, and count reflects
// what was actually stored.
import { handleWaitlistPost, handleWaitlistCount } from "../../worker.js";

let failures = 0;
function check(label, cond) {
  if (!cond) { console.log(`FAIL: ${label}`); failures++; }
  else console.log(`ok: ${label}`);
}

// Fake KV: just enough of the real binding's shape (put + paginated list)
// for these handlers to run against, nothing more.
function makeFakeKV() {
  const store = new Map();
  return {
    store,
    async get(key) { return store.has(key) ? store.get(key) : null; },
    async put(key, value) { store.set(key, value); },
    async list({ cursor } = {}) {
      // Single page every time: real KV pages at 1000 keys, no test here
      // comes close, so one page always has list_complete true.
      return { keys: Array.from(store.keys()).map(name => ({ name })), list_complete: true, cursor: undefined };
    },
  };
}

function postReq(body, headers = {}) {
  const encoded = typeof body === "string" ? body : JSON.stringify(body);
  return new Request("https://joshuatree.heyitsmejosh.com/api/waitlist", {
    method: "POST",
    headers: { "content-type": "application/json", "content-length": String(Buffer.byteLength(encoded)), ...headers },
    body: encoded,
  });
}

{
  const env = { WAITLIST: makeFakeKV() };
  const resp = await handleWaitlistPost(postReq({ email: "Joshua@Example.com" }), env);
  const body = await resp.json();
  check("valid email accepted (200)", resp.status === 200 && body.ok === true);
  check("email stored lowercased as the key", env.WAITLIST.store.has("joshua@example.com"));
  const stored = env.WAITLIST.store.get("joshua@example.com");
  check("value is a real ISO timestamp", !Number.isNaN(Date.parse(stored)));
}

{
  const env = { WAITLIST: makeFakeKV() };
  const resp = await handleWaitlistPost(postReq({ email: "not-an-email" }), env);
  check("malformed email rejected (400)", resp.status === 400);
  check("nothing stored for the rejected email", env.WAITLIST.store.size === 0);
}

{
  const env = { WAITLIST: makeFakeKV() };
  const resp = await handleWaitlistPost(postReq({ email: "" }), env);
  check("empty email rejected (400)", resp.status === 400);
}

{
  const env = { WAITLIST: makeFakeKV() };
  const longLocal = "a".repeat(250) + "@example.com"; // over the 254-char cap
  const resp = await handleWaitlistPost(postReq({ email: longLocal }), env);
  check("email over 254 chars rejected (400)", resp.status === 400);
}

{
  const env = { WAITLIST: makeFakeKV() };
  const resp = await handleWaitlistPost(postReq("not json"), env);
  check("non-JSON body rejected (400)", resp.status === 400);
}

{
  const env = { WAITLIST: makeFakeKV() };
  // Over the 1 KB body cap, declared honestly in content-length.
  const oversized = JSON.stringify({ email: "a@example.com", junk: "x".repeat(2000) });
  const resp = await handleWaitlistPost(postReq(oversized), env);
  check("body over 1KB rejected (413)", resp.status === 413);
  check("oversized body never reaches KV", env.WAITLIST.store.size === 0);
}

{
  // content-length lied about, but the real body is still over the cap:
  // the handler must re-check the actual bytes, not trust the header alone.
  const env = { WAITLIST: makeFakeKV() };
  const oversized = JSON.stringify({ email: "a@example.com", junk: "x".repeat(2000) });
  const req = postReq(oversized, { "content-length": "10" });
  const resp = await handleWaitlistPost(req, env);
  check("oversized body rejected even with a lying content-length (413)", resp.status === 413);
}

{
  const env = { WAITLIST: makeFakeKV() };
  await handleWaitlistPost(postReq({ email: "dupe@example.com" }), env);
  await handleWaitlistPost(postReq({ email: "DUPE@Example.com" }), env);
  check("duplicate signup overwrites, doesn't grow the namespace", env.WAITLIST.store.size === 1);
}

{
  const env = { WAITLIST: makeFakeKV() };
  await handleWaitlistPost(postReq({ email: "one@example.com" }), env);
  await handleWaitlistPost(postReq({ email: "two@example.com" }), env);
  await handleWaitlistPost(postReq({ email: "bad" }), env); // rejected, shouldn't count
  const resp = await handleWaitlistCount(env);
  const body = await resp.json();
  check("count reflects only the real stored signups", resp.status === 200 && body.count === 2);
}

{
  const resp = await handleWaitlistPost(postReq({ email: "a@example.com" }), {});
  check("missing WAITLIST binding fails safe (500), not a crash", resp.status === 500);
}

{
  // the confirmation email once claimed "26 apps" long after the count moved; never hard-code it
  const src = readFileSync(new URL("../../worker.js", import.meta.url), "utf8");
  const mail = src.slice(src.indexOf("const WAITLIST_MAIL_TEXT"), src.indexOf("async function sendWaitlistEmail"));
  check("confirmation email hard-codes no app count", !/\b\d+\s+apps\b/i.test(mail));
}

// Exercise the shipped inline form handler without sending real signups or mail.
{
  const originalFetch = globalThis.fetch;
  const deliveries = [];
  globalThis.fetch = async (url, options) => {
    deliveries.push({ url, ...JSON.parse(options.body) });
    return new Response('{}', { status: 200 });
  };
  try {
    const env = { WAITLIST: makeFakeKV(), RESEND_API_KEY: "test-only" };
    await handleWaitlistPost(postReq({ email: "welcome@example.com" }), env);
    await handleWaitlistPost(postReq({ email: "welcome@example.com" }), env);
    check("only the first signup sends a welcome email", deliveries.length === 1);
    const mail = deliveries[0];
    check("welcome email addresses the signup and includes HTML plus plain text", mail.to[0] === "welcome@example.com" && mail.html.includes("Welcome to") && mail.text.includes("dev kit waitlist"));
    check("welcome email has a preheader, branded mark and presentation tables", mail.html.includes("mso-hide:all") && mail.html.includes("/mark.png") && mail.html.includes('role="presentation"'));
    check("welcome email links to the current film in both versions", [mail.html, mail.text].every(body => body.includes("1.9.32/joshua-tree-ad-v7.mp4") && !body.includes("ad-v6")));
  } finally { globalThis.fetch = originalFetch; }
}

const page = readFileSync(new URL("../../landing/index.html", import.meta.url), "utf8");
const client = page.slice(page.indexOf("(function () {", page.indexOf("// Dev-kit waitlist:")), page.lastIndexOf("</script>", page.indexOf('<script src="v86/libv86.js"')));
async function submitEmail(email, response) {
  let submit, sent = 0, reset = false;
  const status = { textContent: "", setAttribute() {}, removeAttribute() {} };
  const input = { value: email, maxLength: 254 };
  const button = { disabled: false };
  const form = { addEventListener(event, fn) { submit = fn; }, reset() { reset = true; input.value = ""; } };
  const elements = { "waitlist-form": form, "waitlist-email": input, "waitlist-submit": button, "waitlist-status": status };
  runInNewContext(client, {
    document: { getElementById(id) { return elements[id]; } },
    fetch: async () => { sent++; if (response instanceof Error) throw response; return response; },
  });
  submit({ preventDefault() {} });
  await new Promise(resolve => setImmediate(resolve));
  return { text: status.textContent, sent, reset, disabled: button.disabled, value: input.value };
}
for (const email of ["", "   ", "not-an-email", "a@b", "a@@b.com", "x".repeat(250) + "@example.com"]) {
  const result = await submitEmail(email, { ok: true, status: 200 });
  check(`form rejects ${JSON.stringify(email.slice(0, 25))} before sending`, result.sent === 0 && !result.reset && !result.disabled);
}
for (const code of [400, 429, 500, 503]) {
  const result = await submitEmail("joshua@example.com", { ok: false, status: code });
  check(`form explains HTTP ${code} correctly and allows retry`,
    result.sent === 1 && !result.reset && !result.disabled && result.value === "joshua@example.com" &&
    (code === 400 ? result.text.includes("email doesn't look right") : result.text.includes("Could not add you")));
}
{
  const result = await submitEmail("  joshua@example.com  ", { ok: true, status: 200 });
  check("successful form submission resets the email and enables the button", result.sent === 1 && result.reset && !result.disabled && result.text.includes("You're on the list"));
  const offline = await submitEmail("joshua@example.com", new Error("offline"));
  check("offline form submission preserves the email and allows retry", !offline.reset && !offline.disabled && offline.text.includes("Could not reach the server"));
}

if (failures > 0) {
  console.log(`FAIL: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("PASS: /api/waitlist stores a valid, lowercased email keyed by address (dupes overwrite), rejects bad shapes and oversized bodies, and /api/waitlist/count reports the real stored count");
