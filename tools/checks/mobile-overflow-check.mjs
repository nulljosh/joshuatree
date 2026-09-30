// Mobile landing roadmap item ("the landing page on a phone"): the page
// must never grow wider than the viewport at real phone widths. A grid
// column sizing to its content's min-content instead of its fr share
// (the facts-row "v1.6.20" fix this check guards) is exactly the kind of
// bug that never shows up at a desktop width and ships silently.
//
// Loads landing/index.html from a local static server (same pattern as
// hero-contrast-check.mjs) at two real phone widths and fails if
// document.documentElement.scrollWidth exceeds clientWidth at either one.
import { chromium } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml',
                '.wasm': 'application/wasm', '.png': 'image/png', '.txt': 'text/plain' };
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
const browser = await chromium.launch(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
try {
  for (const [w, h] of [[390, 844], [360, 780]]) {
    const page = await browser.newPage({ viewport: { width: w, height: h } });
    await page.goto(url, { waitUntil: 'load' });
    await page.waitForTimeout(500);
    const { scrollWidth, clientWidth } = await page.evaluate(() => ({
      scrollWidth: document.documentElement.scrollWidth,
      clientWidth: document.documentElement.clientWidth,
    }));
    console.log(`${w}x${h}: scrollWidth=${scrollWidth} clientWidth=${clientWidth}`);
    if (scrollWidth > clientWidth) fail(`horizontal overflow at ${w}px (page is ${scrollWidth}px wide)`);
    await page.close();
  }
  if (process.exitCode !== 1) console.log('PASS: no horizontal overflow at 390px or 360px');
} finally {
  await browser.close();
  server.close();
}
