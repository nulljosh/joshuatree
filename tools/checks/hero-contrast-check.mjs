// v0.76.56: three real landing-page bugs, one check.
//  1. The H1 that ships in the HTML (injected from roadmap.md's **Latest**
//     by tools/gen/inject-landing-headline.sh) was being overwritten a
//     second after load by embed.js's hardcoded 'Joshua Tree.' reset, so
//     every visitor watched the real announcement get replaced by a
//     different headline. Asserts the reset types the injected text back.
//  2. .hero h1 was a fixed #ffffff, left over from when the hero copy sat
//     on the demo's dark photo. It sits on the page background now, so
//     light-mode visitors got white-on-#faf8f6. Asserts real contrast in
//     a forced light-scheme context.
//  3. The eyebrow's typewriter rewrites its text every few seconds and the
//     items aren't the same length, so the h2 changed height between them
//     (18px vs 21px at 1440 wide) and pushed every section below it up and
//     down on every cycle. Asserts the h2's height never varies.
//  4. The "Where it's at" progress chart's y-axis tick labels (SVG <text>
//     using fill: var(--muted), tools/gen/progress.sh's color_vars()) were
//     #a39c92 in light mode, 2.72:1 against the progress card's near-white
//     background, a real WCAG AA failure invisible to check #2 above (that
//     one only ever looked at the H1's CSS `color`, not any SVG `fill`).
//     Found via a broader Playwright sweep over every leaf text node on
//     the page (h1/p/labels/buttons/footer/SVG chart text), reading `fill`
//     for SVG text and `color` for everything else. Fixed by darkening
//     --muted to #78736c (4.70:1) in tools/gen/progress.sh, then
//     regenerating progress.svg/landing/progress.svg/the inline chart's
//     <style> block. Asserts the live-rendered tick label's real
//     contrast, not just the source hex value, so a future palette edit
//     that reintroduces a too-light --muted still gets caught.
// Discriminating: revert any one of the four fixes and its own assert fails.
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

const luminance = (rgb) => {
  const [r, g, b] = rgb.match(/[\d.]+/g).slice(0, 3).map(Number).map(v => {
    const c = v / 255;
    return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
};

const fail = (msg) => { console.log('FAIL: ' + msg); process.exitCode = 1; };
const browser = await chromium.launch();
try {
  const page = await browser.newPage({ colorScheme: 'light', viewport: { width: 1440, height: 900 } });
  await page.goto(url);
  const injected = (await page.locator('h1').textContent()).trim();

  // 2. real light-mode contrast between the H1 and whatever is behind it
  const { fg, bg } = await page.evaluate(() => {
    const h1 = document.querySelector('h1');
    let el = h1, bg = 'rgba(0, 0, 0, 0)';
    while (el) {
      const c = getComputedStyle(el).backgroundColor;
      if (c && !/rgba\(0, 0, 0, 0\)|transparent/.test(c)) { bg = c; break; }
      el = el.parentElement;
    }
    return { fg: getComputedStyle(h1).color, bg };
  });
  const [l1, l2] = [luminance(fg), luminance(bg)].sort((a, b) => b - a);
  const ratio = (l1 + 0.05) / (l2 + 0.05);
  console.log(`light mode: h1 ${fg} on ${bg}, contrast ${ratio.toFixed(2)}:1`);
  if (ratio < 4.5) fail(`h1 fails WCAG AA in light mode (${ratio.toFixed(2)}:1, need 4.5:1)`);

  // 4. the progress chart's y-axis tick labels, real SVG fill (not CSS
  // color), against their real card background
  const tickInfo = await page.evaluate(() => {
    const ticks = [...document.querySelectorAll('#progress-chart-live text')]
      .filter(t => /^\d+$/.test(t.textContent.trim()));
    if (!ticks.length) return null;
    const t = ticks[0];
    let el = t, bg = 'rgba(0, 0, 0, 0)';
    while (el) {
      const c = getComputedStyle(el).backgroundColor;
      if (c && !/rgba\(0, 0, 0, 0\)|transparent/.test(c)) { bg = c; break; }
      el = el.parentElement;
    }
    return { fill: getComputedStyle(t).fill, bg, text: t.textContent.trim() };
  });
  if (!tickInfo) {
    fail('no numeric tick label found in the progress chart to check');
  } else {
    const [tl1, tl2] = [luminance(tickInfo.fill), luminance(tickInfo.bg)].sort((a, b) => b - a);
    const tickRatio = (tl1 + 0.05) / (tl2 + 0.05);
    console.log(`light mode: progress chart tick "${tickInfo.text}" ${tickInfo.fill} on ${tickInfo.bg}, contrast ${tickRatio.toFixed(2)}:1`);
    if (tickRatio < 4.5) fail(`progress chart tick label fails WCAG AA in light mode (${tickRatio.toFixed(2)}:1, need 4.5:1)`);
  }

  // 3. the eyebrow keeps its height across a full typewriter cycle
  const heights = new Set();
  // Long enough to cover several typewriter items: the height difference
  // only shows up between items of different length, not within one.
  for (let i = 0; i < 120; i++) {
    heights.add(Math.round((await page.locator('h2.eyebrow').boundingBox()).height));
    await page.waitForTimeout(120);
  }
  console.log('eyebrow heights seen: ' + [...heights].join(', '));
  if (heights.size > 1) fail('eyebrow h2 changes height while typing (page shifts): ' + [...heights].join(', '));

  // 1. the headline the tour resets to is the injected one, not a hardcoded other
  const source = await (await fetch(url.replace('index.html', 'v86/embed.js'))).text();
  if (/typewriterEffect\(h1Link, 'Joshua Tree\.'\)/.test(source))
    fail("resetHeadline still hardcodes 'Joshua Tree.' instead of the injected headline");
  if (!/DEFAULT_HEADLINE/.test(source))
    fail('embed.js no longer derives the default headline from the injected h1');
  console.log(`injected headline: "${injected}"`);
  if (process.exitCode !== 1) console.log('PASS: light-mode contrast, stable eyebrow height, headline reset uses the injected text');
} finally {
  await browser.close();
  server.close();
}
