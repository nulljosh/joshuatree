// 1.1.4: the landing demo's tour cursor glides instead of teleporting
// (direct request). embed.js's moveCursorTo used to send one absolute
// position, so the pointer appeared at its target instantly; it now walks
// an ease-in-out curve at ~60 Hz. This boots the real page in headless
// Chromium (same shape as demochat-check.mjs), waits for the kernel to
// enable the absolute pointer, records every "mouse-absolute" bus send
// during one window.__jt.glideTo() call, and asserts the move was many
// steps, monotonic, timed like a glide, and ended exactly on target.
// Discriminating: with the old one-send moveCursorTo, exactly one
// position is recorded and the step-count assertion fails.
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { execSync } from 'node:child_process';
execSync('make -s kernel.elf', { stdio: 'ignore' }); // landing/v86/kernel.elf is a build artifact the page boots
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.txt': 'text/plain', '.png': 'image/png', '.elf': 'application/octet-stream' };
const server = http.createServer((q, r) => {
  const f = path.join(root, q.url.split('?')[0] === '/' ? 'index.html' : decodeURIComponent(q.url.split('?')[0]));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const url = `http://localhost:${server.address().port}/index.html`;
const CHROMIUM_PATH = process.env.JT_CHROMIUM || (fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined);
const browser = await chromium.launch(CHROMIUM_PATH ? { executablePath: CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.emulateMedia({ reducedMotion: 'reduce' }); // keeps the idle tour from starting mid-check, same as demochat-check
await page.route('**/api/proxy**', route => route.fulfill({ status: 403, body: 'blocked by cursorglide-check' }));
const fails = [];
const ok = m => console.log('  ok:   ' + m);
const fail = m => { console.log('  FAIL: ' + m); fails.push(m); };
try {
  await page.goto(url, { waitUntil: 'load' });
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  ok('window.__jt.ready');
  await page.waitForFunction(() => window.__jt.absolute, null, { timeout: 30000 });
  ok('kernel enabled the absolute pointer (vmmouse backdoor)');
  const r = await page.evaluate(async () => {
    const e = window.__jt.emu;
    e.mouse_adapter.emu_enabled = true;
    const rec = [];
    const orig = e.bus.send.bind(e.bus);
    e.bus.send = (name, data) => { if (name === 'mouse-absolute') rec.push({ t: Date.now(), x: data[0], y: data[1] }); return orig(name, data); };
    window.__jt.moveTo(200, 200);
    await new Promise(res => setTimeout(res, 100));
    rec.length = 0;
    const t0 = Date.now();
    await window.__jt.glideTo(700, 400);
    const t1 = Date.now();
    e.bus.send = orig;
    return { rec, ms: t1 - t0 };
  });
  const n = r.rec.length;
  console.log(`glide (200,200) -> (700,400): ${n} absolute sends over ${r.ms} ms`);
  if (n >= 10) ok(`${n} intermediate positions (a glide, not a jump)`); else fail(`only ${n} position(s) sent: the cursor jumped`);
  const xs = r.rec.map(p => p.x);
  if (xs.every((x, i) => i === 0 || x >= xs[i - 1])) ok('x advanced monotonically'); else fail('x went backwards during the glide');
  const last = r.rec[n - 1] || { x: -1, y: -1 };
  if (Math.abs(last.x - 700.5) < 0.01 && Math.abs(last.y - 400.5) < 0.01) ok('ended exactly on target'); else fail(`ended at (${last.x},${last.y}), not (700.5,400.5)`);
  if (r.ms >= 250 && r.ms <= 1500) ok(`took ${r.ms} ms, a visible glide`); else fail(`took ${r.ms} ms, outside the 250..1500 ms glide window`);
  if (n > 2) { const mid = r.rec[Math.floor(n / 2)].x; if (mid > 200.5 && mid < 700.5) ok('mid-glide position lies between start and target'); else fail(`mid-glide x=${mid} not between start and target`); }
} finally {
  await browser.close();
  server.close();
}
if (fails.length) { console.log('FAIL:'); fails.forEach(f => console.log('  - ' + f)); process.exit(1); }
console.log('PASS: the landing demo\'s tour cursor glides along an eased path to its target instead of teleporting');
