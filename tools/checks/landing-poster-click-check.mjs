#!/usr/bin/env node
// "Tap to boot" must boot: a click on #hero-poster lifts it onto Samantha's face, even when the
// visitor has had the page open a while first.
//
// Bug (2026-10-08): the click only showed "Waking up" and the poster then waited out its whole 60 s
// deadline. facePainted() in landing/v86/embed.js looked for "samopen" and "face: hd=" in serialLog,
// and serialLog keeps only its first 16 KB plus its newest tail once it passes 128 KB. Her periodic
// samface lines push it past that, the markers get cut, and the lift never fires early.
//
// Headless Chromium, in-process static server, hermetic /api/proxy that serves only the committed
// face frames (same shape as hero-poster-check.mjs). For a desktop click and a phone tap:
//   1. wait until the kernel has opened her and loaded her portrait (serial samopen, face: hd=)
//   2. push 140 KB of filler lines through the emulator's own serial0-output-byte bus event, which
//      is exactly what a long-open page does over time, so the 128 KB trim runs
//   3. click or tap the poster: the handler must fire at once (poster gets "waking" or "hidden")
//   4. the poster must be hidden within LIFT_MS, well under the page's own 60 s fallback,
//      and the canvas must be painted (her face is warm)
// Fails on the old indexOf code (step 4 times out); passes once facePainted reads flags.
//
// Usage: node tools/checks/landing-poster-click-check.mjs   (builds kernel.elf first)
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

execSync('make -s kernel.elf', { stdio: 'ignore' });
const root = process.env.LANDING_DIR || path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.webp': 'image/webp', '.jpg': 'image/jpeg', '.png': 'image/png', '.txt': 'text/plain', '.gz': 'application/gzip' };
const server = http.createServer((q, r) => {
  const rel = q.url.split('?')[0];
  const f = path.join(root, rel === '/' ? 'index.html' : decodeURIComponent(rel));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const base = `http://localhost:${server.address().port}/`;
const exe = process.env.CHROMIUM_PATH || process.env.JT_CHROMIUM;
const browser = await chromium.launch({ headless: true, ...(exe ? { executablePath: exe } : {}) });
const fails = [];
const ok = (cond, m) => { console.log((cond ? '  ok:   ' : '  FAIL: ') + m); if (!cond) fails.push(m); return cond; };
const BOOT_MS = 90000;   // v86 boots slowly on a loaded runner
const LIFT_MS = 20000;   // her face is already painted when we click; the page's own fallback is 60 s

for (const [tag, vp, mobile] of [['desktop click', { width: 1440, height: 900 }, false], ['phone tap', { width: 390, height: 844 }, true]]) {
  const ctx = await browser.newContext({ viewport: vp, hasTouch: mobile, isMobile: mobile });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.route('**/api/proxy**', async route => {
    let u = null;
    try { u = new URL(new URL(route.request().url()).searchParams.get('url') || ''); } catch (e) { /* 403 */ }
    if (u && /^\/face\/((idle|talk)-\d{1,2}|hd)\.jpg$/.test(u.pathname)) {
      const file = path.join(root, 'face', path.basename(u.pathname));
      if (fs.existsSync(file)) return route.fulfill({ status: 200, contentType: 'image/jpeg', headers: { 'Access-Control-Allow-Origin': '*' }, body: fs.readFileSync(file) });
    }
    return route.fulfill({ status: 403, body: '' });
  });
  await page.goto(base);
  const visible = await page.evaluate(() => { const e = document.getElementById('hero-poster'); return !!e && !e.hidden && getComputedStyle(e).display !== 'none'; });
  ok(visible, `${tag}: poster is up before the click`);
  const booted = await page.waitForFunction(() => window.__jt && /samopen/.test(window.__jt.serial) && /face: hd=/.test(window.__jt.serial), null, { timeout: BOOT_MS }).then(() => true, () => false);
  ok(booted, `${tag}: emulator booted to Samantha behind the poster (serial samopen, face: hd=)`);
  await page.waitForTimeout(3000);   // her portrait paints a moment after face: hd=

  // A page left open: fill the serial log past its 128 KB trim through the emulator's own bus.
  // emulator_bus is the CPU's half of v86's paired bus: its send() reaches the page-side listeners embed.js registered
  // with add_listener, the same path a real serial byte takes. (emulator.bus.send would go the other way, to nobody.)
  const trimmed = await page.evaluate(() => {
    const bus = window.__joshuaTreeEmulator && window.__joshuaTreeEmulator.emulator_bus;
    if (!bus) return null;
    const line = 'samface: draws=0 filler for the trim\n';
    for (let i = 0; i < 140000 / line.length; i++) for (let k = 0; k < line.length; k++) bus.send('serial0-output-byte', line.charCodeAt(k));
    return { len: window.__jt.serial.length, hd: /face: hd=/.test(window.__jt.serial) };
  });
  console.log(`  [${tag}] after filler: serial ${trimmed && trimmed.len} bytes, "face: hd=" still in the log: ${trimmed && trimmed.hd}`);
  // Without the trim this check proves nothing: the old code would lift at once too.
  ok(trimmed && !trimmed.hd, `${tag}: the filler reached the 128 KB trim and cut "face: hd=" from the log`);

  if (mobile) await page.tap('#hero-poster'); else await page.click('#hero-poster');
  const fired = await page.evaluate(() => { const e = document.getElementById('hero-poster'); return e.hidden || e.classList.contains('waking'); });
  ok(fired, `${tag}: the click handler fired (poster is waking or hidden right away)`);
  const t0 = Date.now();
  const lifted = await page.waitForFunction(() => document.getElementById('hero-poster').hidden, null, { timeout: LIFT_MS }).then(() => true, () => false);
  ok(lifted, `${tag}: poster lifted in ${lifted ? Date.now() - t0 : '>' + LIFT_MS} ms (want under ${LIFT_MS / 1000} s, the 60 s fallback does not count)`);
  const painted = await page.evaluate(() => {
    const c = document.getElementById('screen_canvas'), g = document.createElement('canvas');
    g.width = 32; g.height = 56; const x = g.getContext('2d'); x.drawImage(c, 0, 0, 32, 56);
    const d = x.getImageData(0, 0, 32, 56).data; let warm = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] > d[i + 2] + 25) warm++;
    return warm / (d.length / 4);
  });
  ok(painted >= 0.5, `${tag}: the canvas under the poster is her face (warm share ${(painted * 100).toFixed(0)}%)`);
  if (errors.length) console.log(`  [${tag}] console errors: ${errors.slice(0, 5).join(' | ')}`);
  await ctx.close();
}

await browser.close();
server.close();
if (fails.length) { console.log('FAIL:'); for (const m of fails) console.log('  - ' + m); process.exit(1); }
console.log('PASS: a click or tap on the poster lifts it onto her face, even after the serial log has been trimmed');
