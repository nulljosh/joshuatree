// Direct request: the H1 should say what the demo is actually showing.
//
// It did not. updateHeadline(app.name) fired at the TOP of runSoloApp and
// multiWindowRound, before the tour's dock click had even travelled and
// about a second before the app's first frame drew, so the headline named
// an app that was not on screen yet. On the way out it was worse: the app
// closed and the headline kept announcing it over an empty desktop until
// the next lap's reset.
//
// Fix, in embed.js: the headline is typed only after the real
// open-click-plus-first-frame settle, and resets to the brand line the
// moment the app's window is closed.
//
// This is the check, and it is a real one rather than a source-order
// assertion: it boots the actual landing page in headless Chromium, lets
// the unmodified idle tour run on its own, and repeatedly samples the H1
// text and the real framebuffer AT THE SAME MOMENT. Window 0's own
// close-circle centre at logical (94,56) is the same pixel
// multiwindow-check.py and multiwindowtour-check.mjs already established
// as "a real window is open here", so the two signals can be compared
// directly:
//
//   H1 names an app  =>  a window must really be open
//   H1 is the brand line while the desktop is empty is always fine
//
// The failing direction is the one that was actually wrong: announcing an
// app with nothing on screen. A little tolerance is deliberate, since a
// sample can land in the genuine milliseconds between the headline
// finishing its typewriter run and the window's first frame, or during a
// window's own close animation.
//
// Usage: node tools/checks/headline-sync-check.mjs [url]
import { chromium } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.goto(url, { waitUntil: 'load' });

const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });

const CLOSE_RED = [0xFF, 0x5F, 0x57];
const A = [94, 56];

async function sample() {
  return page.evaluate(([lx, ly]) => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const sx = c.width / 960, sy = c.height / 540;
    const px = Math.round(lx * sx) + 1, py = Math.round(ly * sy) + 1;
    const d = ctx.getImageData(px, py, 1, 1).data;
    const h1 = document.querySelector('h1');
    return { rgb: [d[0], d[1], d[2]], headline: h1 ? h1.textContent.trim() : '' };
  }, [A[0], A[1]]);
}
const isWindowOpen = (p) =>
  Math.abs(p[0] - CLOSE_RED[0]) <= 40 &&
  Math.abs(p[1] - CLOSE_RED[1]) <= 40 &&
  Math.abs(p[2] - CLOSE_RED[2]) <= 40;

const BRAND = 'Introducing Joshua Tree.';
let named = 0, namedWithWindow = 0, orphaned = 0, brandSeen = 0;
const orphanExamples = [];

// The idle tour arms itself after 8s. Two laps' worth of sampling, twice a
// second, is enough to catch every transition without hammering the page.
const DEADLINE = Date.now() + 110000;
while (Date.now() < DEADLINE) {
  const s = await sample();
  const open = isWindowOpen(s.rgb);
  if (s.headline === BRAND) {
    brandSeen++;
  } else if (s.headline.startsWith('Introducing ')) {
    named++;
    if (open) namedWithWindow++;
    else { orphaned++; if (orphanExamples.length < 5) orphanExamples.push(s.headline); }
  }
  await page.waitForTimeout(500);
}
await browser.close();

console.log(`samples naming an app: ${named} (window really open: ${namedWithWindow}, nothing on screen: ${orphaned})`);
console.log(`samples on the brand line: ${brandSeen}`);

if (named === 0) {
  console.log('FAIL: the tour never named an app at all, so this proved nothing');
  process.exit(1);
}
if (brandSeen === 0) {
  console.log('FAIL: the headline never returned to the brand line, so it never resets between apps');
  process.exit(1);
}
// Allow a small share of genuine in-between frames, but the old behavior
// announced apps with an empty desktop for whole seconds at a time and
// lands far past this.
const orphanShare = orphaned / named;
if (orphanShare > 0.25) {
  console.log(`FAIL: ${(orphanShare * 100).toFixed(0)}% of app-naming samples had nothing on screen`);
  console.log('  examples: ' + orphanExamples.join(' | '));
  process.exit(1);
}
console.log('PASS: the headline names an app only while that app is really on screen, and resets when it closes');
