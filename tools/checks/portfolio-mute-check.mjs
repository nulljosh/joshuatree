// Portfolio voice: on by default, with a mute button at the top right.
// Headless Chromium against landing/index.html?full&portfolio on a local static
// server (no kernel needed, the button is page chrome). Asserts: the button
// exists, sits in the top right corner, starts unmuted; one click mutes it
// (aria-pressed, label, remembered across a reload); a second click unmutes;
// and the dead fullscreen button no longer shows where the mute button lives.
// Without ?portfolio there is no mute button at all (Samantha's page is unchanged).
import { chromium } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml', '.wasm': 'application/wasm', '.png': 'image/png', '.jpg': 'image/jpeg', '.webp': 'image/webp', '.txt': 'text/plain' };
const server = createServer(async (req, res) => {
  const path = join(ROOT, decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  try { const body = await readFile(path); res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' }); res.end(body); }
  catch { res.writeHead(404); res.end('nope'); }
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}/index.html`;
const fails = [];
const fail = (m) => { console.log('FAIL: ' + m); fails.push(m); };
const browser = await chromium.launch(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
try {
  for (const [tag, vp] of [['phone', { width: 390, height: 844 }], ['desktop', { width: 1440, height: 900 }]]) {
    const page = await browser.newPage({ viewport: vp });
    await page.goto(base + '?full&portfolio', { waitUntil: 'load' });
    await page.waitForSelector('#jt-mute', { timeout: 5000 }).catch(() => {});
    const g = () => page.evaluate(() => {
      const m = document.getElementById('jt-mute'); if (!m) return null;
      const r = m.getBoundingClientRect(), e = document.getElementById('demo-exit');
      return { top: r.top, right: innerWidth - r.right, pressed: m.getAttribute('aria-pressed'), label: m.getAttribute('aria-label'), exit: e ? getComputedStyle(e).display : 'none', ls: localStorage.getItem('jt-muted') };
    });
    let s = await g();
    if (!s) { fail(`${tag}: no mute button`); await page.close(); continue; }
    if (s.pressed !== 'false') fail(`${tag}: voice is not on by default (aria-pressed ${s.pressed})`);
    if (s.top > 70 || s.right > 30) fail(`${tag}: mute button is not top right (top ${s.top}, right gap ${s.right})`);
    if (s.exit !== 'none') fail(`${tag}: the dead fullscreen button still shows (${s.exit})`);
    await page.click('#jt-mute'); s = await g();
    if (s.pressed !== 'true' || !/^Unmute/.test(s.label) || s.ls !== '1') fail(`${tag}: one click did not mute and remember it: ${JSON.stringify(s)}`);
    await page.reload({ waitUntil: 'load' }); await page.waitForSelector('#jt-mute'); s = await g();
    if (s.pressed !== 'true') fail(`${tag}: mute was not remembered across a reload`);
    await page.click('#jt-mute'); s = await g();
    if (s.pressed !== 'false' || s.ls !== '0') fail(`${tag}: a second click did not unmute: ${JSON.stringify(s)}`);
    await page.close();
  }
  const plain = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  await plain.goto(base + '?full', { waitUntil: 'load' }); await plain.waitForTimeout(800);
  if (await plain.$('#jt-mute')) fail('a mute button appeared outside portfolio mode');
  await plain.close();
  if (!fails.length) console.log('PASS: portfolio voice is on by default, mute sits top right, remembers itself, and stays out of the other pages');
} finally { await browser.close(); server.close(); }
process.exit(fails.length ? 1 : 0);
