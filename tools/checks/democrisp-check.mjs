// Boots the real landing page in headless Chromium at 1x and 2x, desktop
// and mobile, and asserts the demo canvas fills its frame on at least one
// axis. 1.5.8 to 1.5.13 sized it to exact device-pixel steps for
// crispness, which on a Retina Mac left a 960 px canvas in a 1280 px box
// (1.5.14, "big black borders"). image-rendering must be "pixelated" only
// at an exact 1:1 map; any downscale needs "auto" or text shreds.
// Also saves a 2x screenshot of the demo for a human to eyeball.
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
const fails = [];
const ok = m => console.log('  ok:   ' + m);
const fail = m => { console.log('  FAIL: ' + m); fails.push(m); };

async function checkCase(viewport, dpr, label, shot) {
  const page = await browser.newPage({ viewport, deviceScaleFactor: dpr });
  await page.route('**/api/proxy**', route => route.fulfill({ status: 403, body: 'blocked by democrisp-check' }));
  try {
    await page.goto(url, { waitUntil: 'load' });
    await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
    // Let graphical mode come up and resizeCanvas settle (200ms poll loop in embed.js).
    await page.waitForFunction(() => {
      const c = document.getElementById('screen_canvas');
      return c && c.style.display !== 'none' && c.width > 0;
    }, null, { timeout: 60000 });
    await page.waitForTimeout(400);
    const info = await page.evaluate(() => {
      const c = document.getElementById('screen_canvas');
      const r = c.getBoundingClientRect();
      const b = document.getElementById('screen_container').getBoundingClientRect();
      return { backingW: c.width, backingH: c.height, cssW: r.width, cssH: r.height, boxW: b.width, boxH: b.height, imgRendering: c.style.imageRendering };
    });
    // 1.5.14: fill first. The old contract (integer device-pixel steps only)
    // shrank a 1920 framebuffer to 960 CSS px in a 1280 box on Retina, the
    // "big black borders" report. Now the canvas must reach the frame on at
    // least one axis, and "pixelated" is only allowed at an exact 1:1 map.
    const fill = Math.max(info.cssW / info.boxW, info.cssH / info.boxH);
    const ratio = info.backingW / (info.cssW * dpr);
    console.log(`  [${label}] backing=${info.backingW}x${info.backingH} css=${info.cssW.toFixed(1)}x${info.cssH.toFixed(1)} box=${info.boxW.toFixed(1)}x${info.boxH.toFixed(1)} dpr=${dpr} ratio=${ratio.toFixed(3)} image-rendering=${info.imgRendering}`);
    if (fill >= 0.99) ok(`${label}: canvas fills its frame (${(fill * 100).toFixed(1)}%)`);
    else fail(`${label}: canvas only fills ${(fill * 100).toFixed(1)}% of its frame -- the black-border bug`);
    const oneToOne = Math.abs(ratio - 1) < 0.02;
    const wantRendering = oneToOne ? 'pixelated' : 'auto';
    if (info.imgRendering !== wantRendering) fail(`${label}: expected image-rendering:${wantRendering} at ratio ${ratio.toFixed(3)}, got "${info.imgRendering}"`);
    else ok(`${label}: image-rendering is ${wantRendering}`);
    if (shot) {
      await page.locator('#v86-embed').screenshot({ path: shot }).catch(async () => {
        await page.screenshot({ path: shot }); // fall back to full page if the locator screenshot fails for any reason
      });
      ok(`screenshot saved to ${shot}`);
    }
  } finally {
    await page.close();
  }
}

try {
  await checkCase({ width: 1440, height: 900 }, 2, 'desktop@2x', path.join('/tmp', 'jt-demo-crisp-2x.png'));
  await checkCase({ width: 1440, height: 900 }, 1, 'desktop@1x');
  await checkCase({ width: 390, height: 844 }, 2, 'mobile@2x');
  await checkCase({ width: 390, height: 844 }, 1, 'mobile@1x');
} catch (e) {
  fail('unexpected error: ' + (e && e.stack || e));
} finally {
  await browser.close();
  server.close();
}

console.log(fails.length ? `\n${fails.length} check(s) failed.` : '\nAll checks passed.');
process.exit(fails.length ? 1 : 0);
