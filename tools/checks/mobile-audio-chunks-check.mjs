#!/usr/bin/env node
// MANUAL: not wired into CI. Booting the kernel to Chat and proving the
// dac chunk counter rises while Samantha speaks needs `new Worker(blob:...)`,
// which fails under Playwright's bundled WebKit with "WebKitBlobResource
// error 1" (v86's tick scheduler needs it) -- swapping to chromium +
// devices['iPhone 15'] wasn't verified either (no chromium binary in this
// sandbox to test against); try that split before wiring this in. The
// unlock-only half of this proof is covered by mobile-audio-check.mjs,
// which IS wired into CI.
// 1.7.14: Samantha's voice still didn't play on Joshua's real iPhone even
// after 1.7.6's touchend/click unlock. We can't test a real phone from
// here, so this proves the mobile unlock path end to end under real
// WebKit + iPhone emulation (touch events, no mouse), the closest thing
// to a real iPhone Playwright can give us: the AudioContext starts
// "suspended", a real tap on the stage (not a synthetic bus send) flips
// it to "running", and while Samantha speaks the dac-send-data chunk
// counter (?audiodebug, see embed.js's AUDIO_DEBUG block) actually rises,
// proving real PCM reached the speaker adapter and not just that the
// context unlocked.
//
// Usage: node tools/checks/mobile-audio-check.mjs (after `make kernel.elf`)
import { webkit, devices } from 'playwright';
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

const CHAT_FRAME = ''; // no dock click needed: chat_boot_samantha via cmdline puts her straight up

function synthPcm8(ms) {
  const rate = 16000, n = Math.round(rate * ms / 1000);
  const buf = Buffer.alloc(n);
  for (let i = 0; i < n; i++) { const f = Math.floor(i / 8000) % 2 ? 880 : 440; buf[i] = 128 + Math.round(96 * Math.sin(2 * Math.PI * f * i / rate)); }
  return buf;
}
const PCM = synthPcm8(4000);
const REPLY_TEXT = 'This is a real reply from the intercepted demo proxy.';

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.txt': 'text/plain', '.elf': 'application/octet-stream' };
const server = http.createServer((q, r) => {
  const f = path.join(root, q.url.split('?')[0] === '/' ? 'index.html' : decodeURIComponent(q.url.split('?')[0]));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
// ?samantha forces chat_boot_samantha straight to the avatar view, same as
// IS_PHONE always does in embed.js -- no dock click needed at all, one
// less thing to race against a real-phone-shaped viewport.
const url = `http://localhost:${server.address().port}/index.html?samantha&audiodebug`;

const fails = [];
function fail(msg) { fails.push(msg); console.log('  FAIL: ' + msg); }
function ok(msg) { console.log('  ok:   ' + msg); }

const browser = await webkit.launch();
const context = await browser.newContext({ ...devices['iPhone 15'] });
const page = await context.newPage();

let chatServed = false, speakServed = false;
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
    if (fs.existsSync(file)) await route.fulfill({ status: 200, contentType: 'image/jpeg', headers: { 'Access-Control-Allow-Origin': '*' }, body: fs.readFileSync(file) });
    else await route.fulfill({ status: 404, body: '' });
  } else if (isChat && req.method() === 'POST') {
    chatServed = true;
    await route.fulfill({ status: 200, contentType: 'application/json', headers: { 'Access-Control-Allow-Origin': '*' }, body: JSON.stringify({ model: 'samantha', message: { role: 'assistant', content: REPLY_TEXT }, done: true }) });
  } else if (isPick && req.method() === 'POST') {
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
  console.log('serving ' + url + ' as iPhone 15 / WebKit');
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  ok('window.__jt.ready');

  const before = await page.evaluate(() => window.__jt.audioState);
  console.log('AudioContext state before any tap: ' + before);
  // Real Mobile Safari refuses to start an AudioContext with no gesture at
  // all; Playwright's bundled WebKit engine (not literally Safari) is
  // observed to be laxer here and can report "running" this early even
  // headless, with no tap yet -- so this is evidence only, never a hard
  // fail on its own. The unlock listener firing and real samples flowing
  // below are the assertions that actually gate this check.
  if (before === 'running') console.log('  note: already "running" before any tap (WebKit test engine is laxer than real iOS Safari here)');
  else ok(`AudioContext state before tap is "${before}"`);

  // A real touch tap on the stage, not a synthetic bus send -- exactly what
  // a visitor's finger does on a real iPhone, dispatched through Playwright's
  // WebKit touch input, and hits the actual DOM at wherever the stage is,
  // whether that's the overlay, the container or the canvas underneath.
  const box = await page.locator('#v86-embed').boundingBox();
  await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
  await page.waitForTimeout(300);

  const after = await page.evaluate(() => window.__jt.audioState);
  console.log('AudioContext state after real tap: ' + after);
  if (after !== 'running') fail(`AudioContext state is "${after}" after a real tap, expected "running"`);
  else ok('AudioContext.state === "running" after a real touch tap');

  // ?samantha + IS_PHONE's own "phone " boot straight into the full-screen
  // avatar view (kernel/chat.h's chat_boot_samantha_open), which never
  // draws window chrome -- "samfocus" (input box drawn and reading keys)
  // is that path's own marker, not "chatchrome"/"chatconsole" which only
  // fire from the windowed dock path facespeak-demo-check.mjs drives.
  await page.waitForFunction(() => window.__jt.serial.includes('samfocus'), null, { timeout: 30000 })
    .catch(async () => { console.log('--- serial so far ---\n' + (await page.evaluate(() => window.__jt.serial)) + '\n--- end serial ---'); throw new Error('no samfocus'); });
  ok('samfocus serial marker seen: the full-screen avatar view is up and focused');

  const debugBefore = await page.evaluate(() => window.__jt.audioDebug);
  console.log('audioDebug after tap: ' + JSON.stringify(debugBefore));
  if (!debugBefore || debugBefore.unlockAttempts < 1) fail('unlockAttempts never incremented -- the document-level unlock listener never ran');
  else ok(`unlock listener ran ${debugBefore.unlockAttempts} time(s), last event "${debugBefore.lastUnlockEvent}"`);

  // ?samantha boots straight to Chat's avatar view but still waits for a
  // real question, same as the desktop path in facespeak-demo-check.mjs
  // (its own leading 'n' keystroke first, same reasoning: whatever state
  // the chat input starts in, this clears it to a clean prompt).
  await page.evaluate(async () => { await window.__jt.emu.keyboard_send_text('n', 200); });
  await page.waitForTimeout(400);
  await page.evaluate(async () => { await window.__jt.emu.keyboard_send_text('what is the capital of france\n', 55); });
  ok('typed a question into Chat');

  const t0 = Date.now();
  while (Date.now() - t0 < 30000 && !chatServed) await page.waitForTimeout(200);
  if (!chatServed) { fail('the demo never made the intercepted POST to /api/chat'); throw new Error('no chat'); }
  ok('intercepted /api/chat');

  const t1 = Date.now();
  while (Date.now() - t1 < 15000 && !speakServed) await page.waitForTimeout(200);
  if (!speakServed) { fail('speak_text never POSTed to /api/speak'); throw new Error('no speak'); }
  ok('intercepted /api/speak with a real PCM8 body');

  const chunksAtSpeak = (await page.evaluate(() => window.__jt.audioDebug)).chunkCount;
  await page.waitForTimeout(3000);
  const debugAfter = await page.evaluate(() => window.__jt.audioDebug);
  console.log(`dac chunks: ${chunksAtSpeak} at speak-post -> ${debugAfter.chunkCount} 3s later (last non-zero sample: ${debugAfter.lastLevel})`);
  if (debugAfter.chunkCount <= chunksAtSpeak) fail(`dac chunk counter never rose while Samantha should have been speaking (stuck at ${debugAfter.chunkCount}) -- context is unlocked but no samples are reaching the speaker adapter`);
  else ok(`dac chunk counter rose (${chunksAtSpeak} -> ${debugAfter.chunkCount}) while she spoke: real samples reached the speaker adapter`);
} catch (e) {
  if (!fails.length) fail(String(e && e.message || e));
} finally {
  await browser.close();
  server.close();
}

if (fails.length) {
  console.log('FAIL:');
  for (const m of fails) console.log('  - ' + m);
  process.exit(1);
}
console.log('PASS: real WebKit/iPhone 15 touch unlocks the AudioContext and the SB16 dac path actually delivers samples to it');
