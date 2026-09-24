// MANUAL: needs Playwright's Chromium browser binaries plus a local dev server at localhost:3000 (landing/), neither installed in CI (.github/workflows/check.yml has no npm/playwright step). Run by hand after npm install && npx playwright install.
// v0.76.22: real bug found where tourArmed is never reset if tourLoop exits early,
// preventing the idle tour from ever restarting. This regression test verifies
// that the idle tour DOES start automatically after boot without any user
// interaction, by sampling the window close button to confirm a window is open.
//
// Root cause: startTourWhenReady() sets tourArmed=true when it schedules the
// tour, but if tourLoop() exits for any reason (e.g., adaptersReady check),
// tourArmed is never reset to false. This means the setInterval's check
// `if (tourArmed || focused || prefersReducedMotion) return;` will always
// return early, preventing the tour from ever starting again. The fix adds
// a .finally() handler to reset tourArmed when tourLoop completes/exits.
//
// Usage: node tools/checks/idletour-autoplay-check.mjs [url]
// (serve landing/ over plain HTTP first, e.g. `python3 -m http.server`
// from inside landing/, then pass http://localhost:PORT/index.html --
// the live site works too once deployed.)
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

// Wait for graphical mode (canvas should be visible and boot should complete)
await page.waitForTimeout(2000);

function pixelAt(logicalX, logicalY) {
  return page.evaluate(([lx, ly]) => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const sx = c.width / 960, sy = c.height / 540;
    const px = Math.round(lx * sx) + 1, py = Math.round(ly * sy) + 1;
    const d = ctx.getImageData(px, py, 1, 1).data;
    return [d[0], d[1], d[2]];
  }, [logicalX, logicalY]);
}

function isRed(p) {
  const CLOSE_RED = [0xFF, 0x5F, 0x57];
  return Math.abs(p[0] - CLOSE_RED[0]) <= 12 &&
         Math.abs(p[1] - CLOSE_RED[1]) <= 12 &&
         Math.abs(p[2] - CLOSE_RED[2]) <= 12;
}

// Wait for the idle tour to start. The tour should:
// 1. Detect graphical_mode is true (happens within ~1-2s of page load)
// 2. Wait 6000ms before actually starting
// 3. Open the first app (Mail is opened first per tourLoop)
// Total wait: ~8-10 seconds
// We'll poll for 20 seconds to give it plenty of time
const startTime = Date.now();
const timeout = 20000;
let foundWindow = false;

while (Date.now() - startTime < timeout && !foundWindow) {
  const closeButton = await pixelAt(94, 56); // dock-clicked window close button position
  if (isRed(closeButton)) {
    console.log('DETECTED: Window is open (close button is red) at', Math.round(Date.now() - startTime), 'ms');
    foundWindow = true;
    break;
  }
  await page.waitForTimeout(500);
}

await browser.close();

if (foundWindow) {
  console.log('PASS: idle tour started automatically and opened a window');
  process.exit(0);
} else {
  console.log('FAIL: idle tour did not start within 20 seconds, no window was opened');
  process.exit(1);
}
