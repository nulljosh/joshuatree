// 4.22.0, Joshua's ask: "Remove the demo from the landing page. Just show
// screenshots of all use cases." The default page opens on the desktop shot
// and loads no v86 at all; the live demo is one click away at ?demo, where
// the page is exactly what it was before.
//
// Fails if the default view shows #demo-frame, requests libv86.js or
// embed.js, or leaves out any landing/shots/*.webp; or if ?demo stops
// loading embed.js and showing #v86-embed.
import { chromium } from 'playwright';
import { createServer } from 'http';
import { readFile, readdir } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml',
                '.wasm': 'application/wasm', '.png': 'image/png', '.webp': 'image/webp', '.txt': 'text/plain' };
const server = createServer(async (req, res) => {
  const path = join(ROOT, decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  try {
    const body = await readFile(path);
    res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' });
    res.end(body);
  } catch { res.writeHead(404).end('nope'); }
});
await new Promise(r => server.listen(0, r));
const url = `http://127.0.0.1:${server.address().port}/index.html`;

const fail = (msg) => { console.log('FAIL: ' + msg); process.exitCode = 1; };
const shots = (await readdir(join(ROOT, 'shots'))).filter(f => f.endsWith('.webp'));
const browser = await chromium.launch(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
try {
  for (const [mode, q] of [['default', ''], ['demo', '?demo']]) {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    const asked = [];
    page.on('request', r => asked.push(r.url()));
    await page.goto(url + q, { waitUntil: 'load' });
    const v86 = asked.some(u => /libv86\.js|embed\.js/.test(u));
    const s = await page.evaluate(() => {
      const shown = (sel) => { const e = document.querySelector(sel); return !!e && getComputedStyle(e).display !== 'none' && e.getBoundingClientRect().height > 0; };
      return { frame: shown('#demo-frame'), embed: shown('#v86-embed'), hero: shown('.hero-shot img'),
               srcs: [...document.querySelectorAll('img')].map(i => i.getAttribute('src')) };
    });
    console.log(`${mode}: demo-frame=${s.frame} hero=${s.hero} v86=${v86}`);
    if (mode === 'default') {
      if (s.frame) fail('the default page still shows the demo');
      if (v86) fail('the default page loads libv86.js or embed.js');
      if (!s.hero) fail('the default page has no desktop shot hero');
      for (const f of shots) if (!s.srcs.includes('shots/' + f)) fail('no tile for shots/' + f);
    } else {
      if (!s.embed) fail('?demo does not show #v86-embed');
      if (!asked.some(u => /embed\.js/.test(u))) fail('?demo does not load embed.js');
      if (s.hero) fail('?demo still shows the screenshot hero');
    }
    await page.close();
  }
} finally {
  await browser.close();
  server.close();
}
if (!process.exitCode) console.log(`PASS: screenshots by default (${shots.length} shots), live demo at ?demo`);
