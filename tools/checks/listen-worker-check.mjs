#!/usr/bin/env node
// worker.js's /api/listen (Cloudflare Workers AI Whisper): a real Worker
// isn't spun up in this CI, same convention worker-proxy-check.mjs already
// established -- handleListen is exported and plain JS, testable directly
// under Node with a fake env.AI standing in for the real binding.
import assert from "node:assert/strict";
import { handleListen, wrapPcmAsWav, LISTEN_MAX_BYTES } from "../../worker.js";

function req(body, headers = {}) {
  const h = new Headers({ "content-length": String(body ? body.byteLength : 0), ...headers });
  return new Request("https://joshuatree.heyitsmejosh.com/api/listen", {
    method: "POST",
    headers: h,
    body,
  });
}

async function main() {
  // 1. No [ai] binding -> 503, not a crash on env.AI.run of undefined.
  {
    const res = await handleListen(req(new Uint8Array(64)), {});
    assert.equal(res.status, 503, "missing AI binding should 503");
    const body = await res.json();
    assert.ok(body.error, "503 body should explain why");
  }

  // 2. Real (fake) AI binding returns recognized text.
  {
    const fakeAI = { run: async (model, input) => {
      assert.equal(model, "@cf/openai/whisper");
      assert.ok(Array.isArray(input.audio), "audio must be a plain byte array for the AI binding");
      // wrapPcmAsWav should have added a 44-byte RIFF/WAVE header.
      assert.equal(input.audio[0], 0x52); assert.equal(input.audio[1], 0x49);
      return { text: "hello samantha" };
    }};
    const pcm = new Uint8Array(3200).fill(140); // 0.2s of 16kHz 8-bit mono
    const res = await handleListen(req(pcm), { AI: fakeAI });
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.equal(body.text, "hello samantha");
  }

  // 3. Oversized body is rejected before it ever reaches env.AI.run.
  {
    const fakeAI = { run: async () => { throw new Error("should not be called"); } };
    const big = new Uint8Array(LISTEN_MAX_BYTES + 1);
    const res = await handleListen(req(big), { AI: fakeAI });
    assert.equal(res.status, 413);
  }

  // 4. Empty body is rejected.
  {
    const fakeAI = { run: async () => { throw new Error("should not be called"); } };
    const res = await handleListen(req(new Uint8Array(0)), { AI: fakeAI });
    assert.equal(res.status, 400);
  }

  // 5. A body that already looks like a WAV (RIFF header) is passed through
  //    unwrapped, not double-wrapped.
  {
    const already = wrapPcmAsWav(new Uint8Array(100).fill(128), 16000, 8, 1);
    const fakeAI = { run: async (model, input) => {
      assert.equal(input.audio.length, already.length, "an already-WAV body must not be re-wrapped");
      return { text: "ok" };
    }};
    const res = await handleListen(req(already), { AI: fakeAI });
    assert.equal(res.status, 200);
  }

  // 6. Per-IP rate limit binding, when present, is honoured.
  {
    const fakeAI = { run: async () => ({ text: "x" }) };
    let calls = 0;
    const limiter = { limit: async () => { calls++; return { success: calls <= 1 }; } };
    const env = { AI: fakeAI, LISTEN_RATE_LIMITER: limiter };
    const pcm = new Uint8Array(64).fill(128);
    const first = await handleListen(req(pcm, { "cf-connecting-ip": "1.2.3.4" }), env);
    assert.equal(first.status, 200);
    const second = await handleListen(req(pcm, { "cf-connecting-ip": "1.2.3.4" }), env);
    assert.equal(second.status, 429, "second call from the same IP should be rate-limited");
  }

  console.log("listen-worker-check: OK, /api/listen 503s without AI, transcribes, rejects oversize/empty, wraps raw PCM, rate-limits");
}

main().catch(e => { console.error("FAIL:", e.message); process.exit(1); });
