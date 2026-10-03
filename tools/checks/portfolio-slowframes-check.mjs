#!/usr/bin/env node
// The portfolio tour must survive slow face downloads. After the intro video the page sends Escape to close his
// chat and start the tour. If his face frames are still downloading, the app is busy and can miss that key; the
// page has to notice (the kernel logs "ring3app: SAMANTHA.BIN exited") and send it again. Before 2.6.9 it sent it
// once, and on a slow connection the visitor sat on a desktop with no app ever opening.
//
// This boots ?full&portfolio on a desktop with his frames held back and drops the first two Escape presses on the
// floor, exactly what a busy app does, then asserts, off the kernel's own log, that his chat exits and Epiphany
// opens. Discriminating: take the Escape retry out of embed.js (escClose) and this fails, because one Escape is all
// the old code sent.
//
// Usage: node tools/checks/portfolio-slowframes-check.mjs   (after `make kernel.elf`)
import { chromium } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml', '.wasm': 'application/wasm', '.png': 'image/png', '.jpg': 'image/jpeg', '.mp4': 'video/mp4', '.gz': 'application/gzip', '.elf': 'application/octet-stream' };
const server = createServer(async (req, res) => {
  const url = req.url.split('?')[0];
  if (url.startsWith('/face-joshua/') && url.endsWith('.jpg')) await new Promise(r => setTimeout(r, 400));   // a slow connection
  const path = join(ROOT, decodeURIComponent(url).replace(/^\/+/, '') || 'index.html');
  try { const body = await readFile(path); res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' }); res.end(body); }
  catch { res.writeHead(404); res.end('nope'); }
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}/index.html`;
const fails = [];
const fail = (m) => { console.log('FAIL: ' + m); fails.push(m); };
const browser = await chromium.launch(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH, args: ['--autoplay-policy=no-user-gesture-required'] } : { args: ['--autoplay-policy=no-user-gesture-required'] });
try {
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  // The test browser has no H.264, so the real intro video would error out and skip the very moment under test. Make it
  // play silently and report "ended" 20 s in, which is while the slow frames are still coming down.
  await page.addInitScript(() => {
    const add = EventTarget.prototype.addEventListener;
    HTMLVideoElement.prototype.addEventListener = function (t, f, o) { if (t === 'error') return; return add.call(this, t, f, o); };
    Object.defineProperty(HTMLVideoElement.prototype, 'ended', { get() { return performance.now() > 20000; } });
    HTMLVideoElement.prototype.play = function () { return Promise.resolve(); };
  });
  await page.goto(base + '?full&portfolio', { waitUntil: 'load' });
  await page.waitForFunction(() => window.__jt && window.__jt.emu && window.__jt.emu.keyboard_send_keys, null, { timeout: 30000 });
  await page.evaluate(() => {   // a busy app misses the key: drop the first two Escape presses
    const e = window.__jt.emu, send = e.keyboard_send_keys.bind(e); window.__escDropped = 0;
    e.keyboard_send_keys = function (codes, ms) { if (codes.length === 1 && codes[0] === 27 && window.__escDropped < 2) { window.__escDropped++; return Promise.resolve(); } return send(codes, ms); };
  });
  const serial = () => page.evaluate(() => { const j = window.__jt; const x = j && (j.serial || j.serialLog); return typeof x === 'function' ? x() : String(x || ''); });
  let log = '', exited = false, epiphany = false;
  for (let i = 0; i < 240 && !(exited && epiphany); i++) {   // up to two minutes: the intro video alone is 28 s
    await page.waitForTimeout(500);
    log = await serial();
    exited = /ring3app: SAMANTHA\.BIN exited/.test(log);
    epiphany = /ring3app: launching EPIPHANY\.BIN/.test(log);
  }
  const dropped = await page.evaluate(() => window.__escDropped);
  if (!dropped) fail('the check never saw an Escape press, so it proved nothing');
  else if (!exited) fail('his chat never closed after the intro video while his frames were still downloading (the Escape was lost)');
  else if (!epiphany) fail('his chat closed but the tour never opened Epiphany');
  else console.log('ok: with every frame held back 400 ms his chat closed and the tour opened Epiphany');
} finally { await browser.close(); server.close(); }
if (fails.length) process.exit(1);
console.log('PASS: the portfolio tour starts even when his face frames are still downloading');
