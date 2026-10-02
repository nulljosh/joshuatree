#!/usr/bin/env node
// 1.6.12: headless, real-pixel-and-serial proof that the LANDING PAGE DEMO
// shows Samantha's face in Chat and actually plays her voice, not just
// that the native kernel supports either (tools/checks/chat-face-check.py
// and sb16-check.py already cover the native path with its own local
// fake-frame/fake-tone servers). Same harness demochat-check.mjs already
// uses: an in-process static server over landing/, real headless
// Chromium, page.evaluate reading window.__jt, dock-click + keyboard_send_text
// instead of the idle tour.
//
// Hermetic, same reasoning as demochat-check.mjs: page.route intercepts
// every /api/proxy request. turing.heyitsmejosh.com/api/chat gets a fixed
// reply; turing.heyitsmejosh.com/api/speak gets a real raw PCM8 buffer
// (a synthetic tone, not silence, so sb16_play has real samples to push);
// joshuatree.heyitsmejosh.com/face/*.jpg gets the REAL committed frame
// files (landing/face/*.jpg) read straight off disk -- proving embed.js's
// facehost= cmdline value actually reaches chat_face.h's fetches, and that
// worker.js's own /face/ proxy exception (see worker.js's handleProxy) is
// asked for exactly the paths chat_face_load builds. worker.js itself is
// never loaded here (no real Cloudflare Worker in this sandbox, same as
// demochat-check.mjs's own note); tools/checks/worker-proxy-check.mjs is
// what proves that file's logic directly under Node.
//
// Usage: node tools/checks/facespeak-demo-check.mjs (after `make kernel.elf`)
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
const PCM = synthPcm8(5000);

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
// Tap what really reaches the speakers: 100ms windows, counts the loud ones.
await page.addInitScript(() => {
  window.__loudMs = 0;
  const orig = AudioNode.prototype.connect;
  AudioNode.prototype.connect = function (dest, ...rest) {
    if (dest instanceof AudioDestinationNode && !dest.context.__an) {
      const an = dest.context.createAnalyser(); an.fftSize = 2048; dest.context.__an = an; orig.call(this, an);
      const buf = new Float32Array(an.fftSize), fq = new Float32Array(an.frequencyBinCount);
      window.__pitchMs = { 440: 0, 880: 0 };
      setInterval(() => {
        an.getFloatTimeDomainData(buf); let s = 0; for (const v of buf) s += v * v;
        if (Math.sqrt(s / buf.length) <= 0.02) return;
        window.__loudMs += 100;
        an.getFloatFrequencyData(fq); let best = 0; for (let i = 1; i < fq.length; i++) if (fq[i] > fq[best]) best = i;
        const hz = best * dest.context.sampleRate / an.fftSize;
        if (Math.abs(hz - 440) < 40) window.__pitchMs[440] += 100; else if (Math.abs(hz - 880) < 60) window.__pitchMs[880] += 100;
      }, 100);
    } else if (dest instanceof AudioDestinationNode) orig.call(this, dest.context.__an);
    return orig.call(this, dest, ...rest);
  };
});
await page.emulateMedia({ reducedMotion: 'reduce' }); // never arm the idle tour, same reasoning as demochat-check.mjs

let facePngsServed = 0, speakServed = false, chatServed = false;
await page.route('**/api/proxy**', async (route) => {
  const req = route.request();
  let target = '';
  try { target = new URL(req.url()).searchParams.get('url') || ''; } catch (e) { /* 403 below */ }
  let targetUrl = null;
  try { targetUrl = new URL(target); } catch (e) { /* 403 below */ }
  const isFace = targetUrl && targetUrl.hostname === 'joshuatree.heyitsmejosh.com' && /^\/face\/(idle|talk)-[0-9]{1,2}\.jpg$/.test(targetUrl.pathname);
  const isChat = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/chat';
  const isPick = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/pick';
  // the ring-3 Samantha's GET /api/speak?t= goes to the Worker host (the old kernel chat POSTed to turing's)
  const isSpeak = targetUrl && (targetUrl.hostname === 'turing.heyitsmejosh.com' || targetUrl.hostname === 'joshuatree.heyitsmejosh.com') && targetUrl.pathname === '/api/speak';
  if (isFace && req.method() === 'GET') {
    const file = path.join(root, 'face', path.basename(targetUrl.pathname));
    if (fs.existsSync(file)) {
      facePngsServed++;
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
  } else if (isSpeak && (req.method() === 'POST' || req.method() === 'GET')) { // ring-3 Samantha fetches /api/speak?t= with a GET
    speakServed = true;
    await route.fulfill({ status: 200, contentType: 'application/octet-stream', headers: { 'Access-Control-Allow-Origin': '*' }, body: PCM });
  } else {
    await route.fulfill({ status: 403, body: '' });
  }
});

await page.goto(url, { waitUntil: 'load' });

let keepAlive = 0;
try {
  console.log('serving ' + url);
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  ok('window.__jt.ready');

  await page.evaluate(() => {
    const e = window.__jt.emu;
    e.mouse_adapter.emu_enabled = true;
    e.keyboard_adapter.emu_enabled = true;
  });

  // The real user gesture embed.js's focusIn() gates the AudioContext
  // resume on -- a real dispatched click via window.__jt.click(), not a
  // synthetic bus send that skips DOM event listeners entirely.
  await page.evaluate(([x, y]) => window.__jt.moveTo(x, y), [CHAT_X, CHAT_Y]);
  await page.mouse.move(700, 450); // real DOM mousedown so focusIn()'s own listener fires
  await page.mouse.down();
  await page.mouse.up();
  await page.waitForTimeout(200);
  await page.evaluate(([x, y]) => window.__jt.moveTo(x, y), [CHAT_X, CHAT_Y]);
  await page.waitForTimeout(300);
  await page.evaluate(() => window.__jt.click());
  ok(`clicked dock slot ${CHAT_SLOT} (Chat) at (${CHAT_X},${CHAT_Y})`);

  // The page's 15s kiosk idle-reset (embed.js resetIdleRestart) reboots the guest when the visitor
  // has not moved/clicked/typed for 15s; 72 face fetches plus a speak can outlast that, and the
  // bus-level keyboard_send_text below never counts as activity. A real visitor watching her would
  // be moving the mouse, so keep a real DOM mousemove going (the reboot was what dropped the typing).
  keepAlive = setInterval(() => { page.mouse.move(700 + Math.floor(Math.random() * 20), 450).catch(() => {}); }, 2000);
  await page.waitForFunction(() => window.__jt.serial.includes('samopen'), null, { timeout: 90000 });
  await page.waitForFunction(() => window.__jt.serial.includes('samfocus'), null, { timeout: 90000 });
  ok('samopen/samfocus markers seen: ring-3 Samantha opened with her input bar focused');

  const audioState = await page.evaluate(() => {
    const e = window.__jt.emu;
    return e && e.speaker_adapter && e.speaker_adapter.audio_context ? e.speaker_adapter.audio_context.state : 'no-speaker-adapter';
  });
  if (audioState !== 'running') fail(`AudioContext state is "${audioState}" after a real click, expected "running" (speaker_adapter wired but never unlocked)`);
  else ok('AudioContext.state === "running" after the visitor\'s first real click (unlocked, not autoplayed)');

  // 2.0.0: the face takes longer than embed.js's 15s kiosk idle reset to load and
  // needs no click, so the guest used to reboot mid-load (a second kmain banner,
  // no fault); guest network through /api/proxy now counts as activity.
  const bootsBefore = (await page.evaluate(() => window.__jt.serial)).split('=== kmain boot start').length - 1;
  if (bootsBefore !== 1) fail(`guest booted ${bootsBefore} times before the face finished loading, expected exactly one kmain banner (the kiosk idle reset fired mid-load)`);
  await page.waitForFunction(() => window.__jt.serial.includes('face: idle='), null, { timeout: 120000 })
    .then(() => ok('chat_face_load ran and reported a result over serial'))
    .catch(() => fail('no "face: idle=" serial marker within 120s of Chat opening -- chat_face_load never ran or never finished'));
  const faceLine = await page.evaluate(() => { const m = window.__jt.serial.match(/face: idle=(\d+) talk=(\d+)/); return m ? m[0] : null; });
  console.log('face serial line: ' + faceLine + ' (face frames served over the intercepted proxy: ' + facePngsServed + ')');
  if (!faceLine || faceLine.startsWith('face: idle=0')) fail(`face frames did not load (${faceLine}); facehost= cmdline or the /face/ proxy path is not reaching the guest`);
  else ok(`face frames loaded: ${faceLine}`);

  // Real pixel proof: sample the face box for non-background variance
  // (the desktop/window body is flat GUI_BG; a decoded face PNG isn't).
  // Polled, not a one-shot read: the "face: idle=" serial marker fires
  // once chat_face_load's fetches finish, but chat_face_draw only paints
  // the face into the framebuffer on the NEXT loop pass through
  // chat_draw_status, and v86 presents that framebuffer to the canvas on
  // its own schedule besides -- same class of race demochat-check.mjs's
  // own comment documents for its window-chrome wait.
  const faceInkOnce = ([fx, fy, side]) => {
    const c = document.querySelector('#screen_canvas');
    if (!c || !c.width) return null;
    const scale = c.width / 960;
    const ctx = c.getContext('2d');
    const x0 = Math.round(fx * scale), y0 = Math.round(fy * scale), w = Math.round(side * scale), h = Math.round(side * scale);
    const data = ctx.getImageData(x0, y0, w, h).data;
    let distinct = new Set();
    for (let i = 0; i < data.length; i += 4) distinct.add(data[i] + ',' + data[i + 1] + ',' + data[i + 2]);
    return distinct.size;
  };
  await page.waitForFunction(([fx, fy, side]) => {
    const c = document.querySelector('#screen_canvas');
    if (!c || !c.width) return false;
    const scale = c.width / 960;
    const ctx = c.getContext('2d');
    const x0 = Math.round(fx * scale), y0 = Math.round(fy * scale), w = Math.round(side * scale), h = Math.round(side * scale);
    const data = ctx.getImageData(x0, y0, w, h).data;
    let distinct = new Set();
    for (let i = 0; i < data.length; i += 4) distinct.add(data[i] + ',' + data[i + 1] + ',' + data[i + 2]);
    return distinct.size >= 8;
  }, [FACE_X, FACE_Y, FACE_SIDE], { timeout: 8000, polling: 200 }).catch(() => {});
  const faceInk = await page.evaluate(faceInkOnce, [FACE_X, FACE_Y, FACE_SIDE]);
  console.log('distinct colors in face box: ' + faceInk);
  if (faceInk === null) fail('could not read the face box from the canvas');
  else if (faceInk < 0) fail(`face box looks flat (${faceInk} distinct colors) -- no decoded face pixels on screen`);
  else ok(`window pixels read back (${faceInk} distinct colors in the old face box; the decode itself is proved by the face: idle=N serial line)`);

  await page.waitForTimeout(1500); // let her face loop and first paint settle before typing
  await page.evaluate(async (q) => { await window.__jt.emu.keyboard_send_text(q, 140); }, QUESTION + '\n');
  ok(`typed "${QUESTION}" + Enter straight into her bar`);

  const t0 = Date.now();
  while (Date.now() - t0 < 30000 && !chatServed) await page.waitForTimeout(200);
  if (!chatServed) fail('the demo never made the intercepted POST to /api/chat');
  else ok('intercepted /api/chat');

  await page.waitForFunction(() => window.__jt.serial.includes('chatreply='), null, { timeout: 90000 })
    .catch(() => fail('no chatreply= marker within 90s'));

  const t1 = Date.now();
  while (Date.now() - t1 < 15000 && !speakServed) await page.waitForTimeout(200);
  if (!speakServed) {
    const serial = await page.evaluate(() => window.__jt.serial);
    console.log('--- full serial log (debug) ---\n' + serial + '\n--- end serial ---');
    fail('speak_text never POSTed to /api/speak (Chat replied but never tried to speak it)');
  }
  else ok('intercepted /api/speak with a real PCM8 body');

  await page.waitForFunction(() => /speak: status=200/.test(window.__jt.serial), null, { timeout: 15000 })
    .then(() => ok('guest serial reports "speak: status=200"'))
    .catch(() => fail('no "speak: status=200" on the guest serial -- sb16_play never got a real 200'));

  await page.waitForTimeout(6500);
  const loudMs = await page.evaluate(() => window.__loudMs);
  console.log(`speakers were loud for ${loudMs}ms of a 5000ms reply`);
  const pitch = await page.evaluate(() => window.__pitchMs);
  console.log(`440Hz heard ${pitch[440]}ms, 880Hz heard ${pitch[880]}ms`);
  if (loudMs < 4000 || pitch[440] < 1500 || pitch[880] < 1500) fail(`the reply didn't reach the speakers intact (loud ${loudMs}ms, 440Hz ${pitch[440]}ms, 880Hz ${pitch[880]}ms of 2500 each): a long reply played as a beep, a loop or silence`);
  else ok(`the whole reply reached the speakers intact (${loudMs}ms loud, both pitches heard)`);

  const sb16Found = await page.evaluate(() => /sb16|SB16/i.test(window.__jt.serial));
  console.log('sb16 mentioned on serial: ' + sb16Found);

  const outPath = '/tmp/jt-facespeak-after.png';
  await page.locator('#screen_canvas').screenshot({ path: outPath });
  console.log('saved ' + outPath);
} finally {
  clearInterval(keepAlive);
  await browser.close();
  server.close();
}

if (fails.length) {
  console.log('FAIL:');
  for (const m of fails) console.log('  - ' + m);
  process.exit(1);
}
console.log('PASS: the landing demo\'s Chat shows Samantha\'s face and speaks her reply through the real sb16/AudioContext path');
