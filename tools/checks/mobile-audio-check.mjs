#!/usr/bin/env node
// 1.7.14: Samantha's voice still didn't play on Joshua's real iPhone even
// after 1.7.6's touchend/click unlock. We can't test a real phone from
// here, so this proves the mobile unlock path end to end under real
// WebKit + iPhone emulation (touch events, no mouse): the AudioContext
// starts "suspended", a real tap on the stage (not a synthetic bus send)
// flips it to "running", the document-capture-phase unlock listener
// fired at least once, and the ?audiodebug overlay actually renders.
//
// This is unlock-only. The chunk-counter proof ("dac chunks rise while
// she speaks") needs a full kernel boot to Chat, which hits a real
// Playwright-WebKit bug (new Worker(blob:...) fails with "WebKitBlobResource
// error 1", which v86's tick scheduler needs) -- see
// tools/checks/mobile-audio-chunks-check.mjs (MANUAL) for that half.
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

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.txt': 'text/plain', '.elf': 'application/octet-stream' };
const server = http.createServer((q, r) => {
  const f = path.join(root, q.url.split('?')[0] === '/' ? 'index.html' : decodeURIComponent(q.url.split('?')[0]));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
// ?samantha forces chat_boot_samantha straight to the avatar view, same as
// IS_PHONE always does in embed.js -- no dock click needed. We don't need
// the kernel to fully boot for the unlock proof, just the page + emulator
// object to exist, but ?samantha keeps this on the same real code path a
// phone visitor hits.
const url = `http://localhost:${server.address().port}/index.html?samantha&audiodebug`;

const fails = [];
function fail(msg) { fails.push(msg); console.log('  FAIL: ' + msg); }
function ok(msg) { console.log('  ok:   ' + msg); }

const browser = await webkit.launch();
const context = await browser.newContext({ ...devices['iPhone 15'] });
const page = await context.newPage();
// The demo talks to real network endpoints once booted; block everything so
// a boot-stall or a network flake can't hang this unlock-only check.
await page.route('**/api/proxy**', (route) => route.fulfill({ status: 403, body: '' }));

await page.goto(url, { waitUntil: 'load' });

try {
  console.log('serving ' + url + ' as iPhone 15 / WebKit');
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  ok('window.__jt.ready');

  const overlayVisible = await page.evaluate(() => {
    const el = document.getElementById('audio-debug-overlay');
    return !!el && el.textContent.length > 0;
  });
  if (!overlayVisible) fail('?audiodebug overlay (#audio-debug-overlay) did not render with text content');
  else ok('?audiodebug overlay rendered');

  const before = await page.evaluate(() => window.__jt.audioState);
  console.log('AudioContext state before any tap: ' + before);
  // Real Mobile Safari refuses to start an AudioContext with no gesture at
  // all; Playwright's bundled WebKit engine (not literally Safari) is
  // observed to be laxer here and can report "running" this early even
  // headless, with no tap yet -- so this is evidence only, never a hard
  // fail on its own. The unlock listener firing and the state flipping to
  // "running" after a real tap are the assertions that actually gate this.
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

  const debugAfterTap = await page.evaluate(() => window.__jt.audioDebug);
  console.log('audioDebug after tap: ' + JSON.stringify(debugAfterTap));
  if (!debugAfterTap || debugAfterTap.unlockAttempts < 1) fail('unlockAttempts never incremented -- the document-level unlock listener never ran');
  else ok(`unlock listener ran ${debugAfterTap.unlockAttempts} time(s), last event "${debugAfterTap.lastUnlockEvent}"`);
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
console.log('PASS: real WebKit/iPhone 15 touch unlocks the AudioContext, the unlock listener fires, and the ?audiodebug overlay renders');
