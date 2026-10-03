#!/usr/bin/env node
// Locked-audio regression: the browser keeps its AudioContext suspended until
// the visitor's first click, and sb16_play used to give up on a chunk after a
// fixed (chunk length + 1s), so a reply that started while audio was still
// locked played its first two seconds and then went silent ("sb16: transfer
// timed out"). Here the context stays suspended for 6s after she starts
// speaking an 8s reply, then unlocks; the whole reply must still come out and
// the guest must not log a timeout. Hermetic like facespeak-demo-check.mjs.
//
// Usage: node tools/checks/speak-locked-audio-check.mjs (after `make kernel.elf`)
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
const kernelElf = path.join(root, 'v86', 'kernel.elf');
if (!fs.existsSync(kernelElf)) {
  console.error('FAIL: ' + kernelElf + ' does not exist -- run `make kernel.elf` first');
  process.exit(1);
}

const CHROMIUM_PATH = process.env.JT_CHROMIUM || (fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined);

const LOGICAL_W = 960, LOGICAL_H = 540;
const QUESTION = 'what is the capital of france';
const REPLY_TEXT = 'Paris is the capital of France, and this reply came through the demo proxy.';

// Same dock geometry demochat-check.mjs derives from embed.js's dockSlotPos.
function dockSlotPos(slot) {
  const count = 11, gap = 6, pad = 10, marginBot = 24, budget = 740;
  let icon = Math.floor(LOGICAL_H * 7 / 100);
  const maxByWidth = Math.floor((budget - 2 * pad - (count - 1) * gap) / count);
  if (icon > maxByWidth) icon = maxByWidth;
  if (icon < 16) icon = 16;
  const dockW = count * icon + (count - 1) * gap + 2 * pad;
  const x0 = Math.floor((LOGICAL_W - dockW) / 2) + pad + Math.floor(icon / 2);
  const cy = LOGICAL_H - marginBot - pad - Math.floor(icon / 2);
  return [x0 + slot * (icon + gap), cy];
}
const CHAT_SLOT = 7;
const [CHAT_X, CHAT_Y] = dockSlotPos(CHAT_SLOT);
// Chat window viewport + face box, same geometry demochat-check.mjs and
// chat_face.h's chat_face_draw agree on: face sits FACE_SIDE(60)+16 in
// from the right edge of the window, T+44 down (T=gui_app_dy()=-32
// windowed), a 60x60 logical square.
const VX = 78, VY = 72, VW = 804;
const FACE_SIDE = 60;
// Exactly chat_face_draw's own math (kernel/chat_face.h): face_x =
// window_width() - 20 - FACE_SIDE, a LOCAL x where 0 is the window's own
// content left edge (screen x = VX + local x, same convention
// demochat-check.mjs's VX+4.. scan already uses); window_width() reports
// the same VW (804) demochat-check.mjs derives from gui_launch_from_dock's
// viewport. face_y = T+44 with T=gui_app_dy()=-32 windowed, screen y = VY + that.
const FACE_X = VX + (VW - 20 - FACE_SIDE);
const FACE_Y = VY + (-32 + 44);

// A real, non-silent 8-bit mono PCM8 tone at 16kHz -- sb16_play needs
// actual sample variance to prove it "played" something, not just a
// nonzero byte count.
function synthPcm8(ms) {
  const rate = 16000, n = Math.round(rate * ms / 1000);
  const buf = Buffer.alloc(n);
  // 440Hz and 880Hz swap every 500ms: a stuck or looped buffer can't fake both
  for (let i = 0; i < n; i++) { const f = Math.floor(i / 8000) % 2 ? 880 : 440; buf[i] = 128 + Math.round(96 * Math.sin(2 * Math.PI * f * i / rate)); }
  return buf;
}
// 5s = 80000 bytes, past sb16's 32KB DMA chunk so playback spans several
// transfers: v86 turned a single full 64KB transfer into one beep, and only
// a long reply ever hit it.
const PCM = synthPcm8(8000);

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.txt': 'text/plain', '.elf': 'application/octet-stream' };
const server = http.createServer((q, r) => {
  const f = path.join(root, q.url.split('?')[0] === '/' ? 'index.html' : decodeURIComponent(q.url.split('?')[0]));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const url = `http://localhost:${server.address().port}/index.html`;

const fails = [];
function fail(msg) { fails.push(msg); console.log('  FAIL: ' + msg); }
function ok(msg) { console.log('  ok:   ' + msg); }

const browser = await chromium.launch(CHROMIUM_PATH ? { executablePath: CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
// Tap what reaches the speakers, and hold the AudioContext suspended until
// window.__unlock() runs (v86 and embed.js both call resume(); those are no-ops).
await page.addInitScript(() => {
  window.__loudMs = 0;
  const realResume = AudioContext.prototype.resume;
  const ctxs = [];
  AudioContext.prototype.resume = function () { if (window.__unlocked) return realResume.call(this); return Promise.resolve(); };
  window.__unlock = () => { window.__unlocked = true; ctxs.forEach(c => realResume.call(c)); };
  const orig = AudioNode.prototype.connect;
  AudioNode.prototype.connect = function (dest, ...rest) {
    if (dest instanceof AudioDestinationNode && !dest.context.__an) {
      const c = dest.context; ctxs.push(c); if (!window.__unlocked) c.suspend();
      const an = c.createAnalyser(); an.fftSize = 2048; c.__an = an; orig.call(this, an);
      const buf = new Float32Array(an.fftSize);
      setInterval(() => {
        an.getFloatTimeDomainData(buf); let s = 0; for (const v of buf) s += v * v;
        if (Math.sqrt(s / buf.length) > 0.02) window.__loudMs += 100;
      }, 100);
    } else if (dest instanceof AudioDestinationNode) orig.call(this, dest.context.__an);
    return orig.call(this, dest, ...rest);
  };
});
await page.emulateMedia({ reducedMotion: 'reduce' });

let speakServed = false, chatServed = false;
await page.route('**/api/proxy**', async (route) => {
  const req = route.request();
  let target = '';
  try { target = new URL(req.url()).searchParams.get('url') || ''; } catch (e) { /* 403 below */ }
  let targetUrl = null;
  try { targetUrl = new URL(target); } catch (e) { /* 403 below */ }
  const isFace = targetUrl && targetUrl.hostname === 'joshuatree.heyitsmejosh.com' && /^\/face\/(idle|talk)-[0-9]{1,2}\.jpg$/.test(targetUrl.pathname);
  const isChat = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/chat';
  const isPick = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/pick';
  const isSpeak = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/speak';
  if (isFace && req.method() === 'GET') {
    const file = path.join(root, 'face', path.basename(targetUrl.pathname));
    if (fs.existsSync(file)) {
      
      await route.fulfill({ status: 200, contentType: 'image/jpeg', headers: { 'Access-Control-Allow-Origin': '*' }, body: fs.readFileSync(file) });
    } else {
      await route.fulfill({ status: 404, body: '' });
    }
  } else if (isChat && req.method() === 'POST') {
    chatServed = true;
    await route.fulfill({ status: 200, contentType: 'application/json', headers: { 'Access-Control-Allow-Origin': '*' }, body: JSON.stringify({ model: 'samantha', message: { role: 'assistant', content: REPLY_TEXT }, done: true }) });
  } else if (isPick && req.method() === 'POST') {
    // No tool for the capital-of-france question -- falls through to chat_send.
    await route.fulfill({ status: 403, body: '' });
  } else if (isSpeak && req.method() === 'POST') {
    speakServed = true;
    await route.fulfill({ status: 200, contentType: 'application/octet-stream', headers: { 'Access-Control-Allow-Origin': '*' }, body: PCM });
  } else {
    await route.fulfill({ status: 403, body: '' });
  }
});

await page.goto(url, { waitUntil: 'load' });
try {
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  await page.evaluate(() => { const e = window.__jt.emu; e.mouse_adapter.emu_enabled = true; e.keyboard_adapter.emu_enabled = true; });
  await page.evaluate(([x, y]) => window.__jt.moveTo(x, y), [CHAT_X, CHAT_Y]);
  await page.waitForTimeout(500);
  await page.evaluate(() => window.__jt.click());
  await page.waitForFunction(() => window.__jt.serial.includes('chatconsole'), null, { timeout: 20000 });
  ok('Chat opened');
  await page.evaluate(async () => { await window.__jt.emu.keyboard_send_text('n', 200); });
  await page.waitForTimeout(400);
  await page.evaluate(async (q) => { await window.__jt.emu.keyboard_send_text(q, 55); }, QUESTION + '\n');
  await page.waitForFunction(() => /speak: status=200/.test(window.__jt.serial), null, { timeout: 45000 })
    .catch(() => fail('guest never reported speak: status=200'));
  const state = await page.evaluate(() => window.__jt.audioState);
  if (state === 'running') fail('test setup: the AudioContext was not held suspended');
  await page.waitForTimeout(6000); // past the old chunk limit (about 3s) with audio still locked
  await page.evaluate(() => window.__unlock());
  await page.waitForTimeout(11000);
  const serial = await page.evaluate(() => window.__jt.serial);
  const loudMs = await page.evaluate(() => window.__loudMs);
  console.log(`speakers were loud for ${loudMs}ms of an 8000ms reply`);
  if (/transfer timed out/.test(serial)) fail('guest gave up on the reply while audio was locked ("sb16: transfer timed out")');
  else ok('no transfer timeout while audio was locked');
  if (loudMs < 6500) fail(`only ${loudMs}ms of the 8000ms reply was heard after unlocking`);
  else ok(`the whole reply played after the late unlock (${loudMs}ms loud)`);
} finally {
  await browser.close();
  server.close();
}
if (fails.length) { console.log('FAIL:'); for (const m of fails) console.log('  - ' + m); process.exit(1); }
console.log('PASS: a reply that starts while the browser still has audio locked plays in full once it unlocks');
