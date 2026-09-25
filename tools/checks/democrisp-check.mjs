// 1.5.8: real report on a Retina Mac in Safari, "the live demo is still
// pretty pixely." Root cause: the guest framebuffer is a fixed pixel
// resolution, but embed.js's resizeCanvas used to size the canvas's CSS
// box purely against the container's CSS-pixel box (cover/contain), with
// no regard for the real DEVICE pixel grid a Retina screen draws to,
// forcing a fractional resample that smears every glyph. Fixed by sizing
// the canvas so canvas pixels map 1:1 (or an exact integer divisor) to
// device pixels whenever the viewport allows it.
//
// This boots the real page in headless Chromium at deviceScaleFactor 2
// and 1, at desktop (1440) and mobile (390) viewport widths, and asserts
// the canvas's CSS box times devicePixelRatio divides its backing
// resolution by a whole number on both desktop cases (where the box is
// large enough for an exact mapping); it also takes a screenshot of the
// demo area at 2x for a human to eyeball. Discriminating: reverting to
// the old cover/contain-only resizeCanvas produces a non-integer
// device-pixel ratio at 1440x900 @2x (the bug's own reported shape).
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
      return { backingW: c.width, backingH: c.height, cssW: r.width, cssH: r.height, imgRendering: c.style.imageRendering };
    });
    const devPxW = info.cssW * dpr;
    const devPxH = info.cssH * dpr;
    const ratioW = info.backingW / devPxW;
    const ratioH = info.backingH / devPxH;
    const nearInt = v => Math.abs(v - Math.round(v)) < 0.02 && Math.round(v) >= 1;
    console.log(`  [${label}] backing=${info.backingW}x${info.backingH} css=${info.cssW.toFixed(1)}x${info.cssH.toFixed(1)} dpr=${dpr} devicePx=${devPxW.toFixed(1)}x${devPxH.toFixed(1)} ratio=${ratioW.toFixed(3)}x${ratioH.toFixed(3)} image-rendering=${info.imgRendering}`);
    if (nearInt(ratioW) && nearInt(ratioH)) {
      ok(`${label}: canvas backing pixels map to device pixels by an integer factor (${Math.round(ratioW)})`);
      if (info.imgRendering !== 'pixelated') fail(`${label}: expected image-rendering:pixelated for an integer device-pixel mapping, got "${info.imgRendering}"`);
      else ok(`${label}: image-rendering is pixelated for the exact mapping`);
    } else {
      fail(`${label}: canvas is NOT mapped to device pixels by a whole number (ratio ${ratioW.toFixed(3)}x${ratioH.toFixed(3)}) -- this is the pixely-resample bug`);
    }
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
  await checkCase({ width: 1440, height: 900 }, 2, 'desktop@2x', '/private/tmp/claude-501/-Users-joshua/aa570436-1e4c-4fd4-81eb-aba396aba745/scratchpad/demo-crisp-2x.png');
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
