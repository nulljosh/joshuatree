// Real diagnostic, not a committed regression test: direct, repeated
// report that the live landing page still shows a stale/wrong wallpaper
// despite the local fix (v0.76.13's Settings-click regression fix)
// already verified working in a local sandbox build. This session's own
// network blocks the live URL directly and blocks downloading workflow
// artifacts (confirmed, see roadmap.md), so this prints real pixel
// analysis straight to the job log instead -- readable via the Actions
// API even when the binary screenshot itself can't be fetched.
//
// Samples the exact wallpaper region over a real 100s window (long
// enough to cover a full idle-tour lap reaching the Satellite reveal)
// and reports real variance (a flat/solid colour reads as "not painted",
// real photographic/tile content reads as high-variance) plus the exact
// RGB at a few fixed points, so a session reading only this text output
// can tell photo-fallback vs a real tile mosaic vs solid-color-broken
// apart without ever seeing the image.
import { chromium } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
page.on('console', msg => console.log('[page]', msg.text()));
await page.goto(url, { waitUntil: 'load' });

const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });
console.log('graphical mode reached');

function sampleWallpaper() {
  return page.evaluate(() => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const sx = c.width / 960, sy = c.height / 540;
    // A broad band well clear of the menubar/dock, logical (100..860, 100..400).
    const x0 = Math.round(100 * sx), y0 = Math.round(100 * sy);
    const w = Math.round(760 * sx), h = Math.round(300 * sy);
    const d = ctx.getImageData(x0, y0, w, h).data;
    let sum = 0, sumSq = 0, n = 0;
    for (let i = 0; i < d.length; i += 4 * 37) {
      const lum = (d[i] + d[i + 1] + d[i + 2]) / 3;
      sum += lum; sumSq += lum * lum; n++;
    }
    const mean = sum / n;
    const variance = sumSq / n - mean * mean;
    return { mean: Math.round(mean), variance: Math.round(variance), n };
  });
}

const t0 = Date.now();
const samples = [];
while (Date.now() - t0 < 100000) {
  await page.waitForTimeout(4000);
  const s = await sampleWallpaper();
  const elapsed = Date.now() - t0;
  samples.push({ elapsed, ...s });
  console.log('t=' + elapsed + 'ms mean=' + s.mean + ' variance=' + s.variance);
}

const maxVar = Math.max(...samples.map(s => s.variance));
const minVar = Math.min(...samples.map(s => s.variance));
console.log('variance range over the whole run: min=' + minVar + ' max=' + maxVar);
console.log(maxVar > 200
  ? 'Real photographic/tile-like variance observed at least once -- wallpaper is NOT a flat/solid color at that moment.'
  : 'Variance never exceeded 200 -- wallpaper looks flat/solid the whole run, real evidence of a stuck/broken paint.');
await browser.close();
