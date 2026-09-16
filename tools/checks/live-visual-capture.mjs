// Not a regression check (no PASS/FAIL assertion) -- a real evidence-
// capture script. Direct request: "figure out a display to verify" the
// satellite wallpaper and the new multi-window demo actually paint
// correctly for a real visitor. This session runs in a cloud sandbox with
// no physical display attached, and its own network egress policy blocks
// joshuatree.heyitsmejosh.com directly (confirmed repeatedly, see
// roadmap.md's CORS-fix entries) -- there's no way to screenshot the real
// live site from in here. GitHub Actions runners have real, unrestricted
// internet access (already proven by check.yml's own `network` job and
// live-smoke.yml), so this runs a real headless Chromium there instead:
// a real video (Playwright's own recordVideo, a genuine .webm, not a
// screenshot slideshow) plus periodic real screenshots of the live idle
// tour, both saved to `outDir` for a workflow run to upload as a real,
// downloadable artifact.
//
// Usage: node tools/checks/live-visual-capture.mjs [url] [outDir] [durationMs]
import { chromium } from 'playwright';
import fs from 'fs';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
const outDir = process.argv[3] || 'capture';
const durationMs = Number(process.argv[4] || 100000);
fs.mkdirSync(outDir, { recursive: true });

const browser = await chromium.launch();
const context = await browser.newContext({
  viewport: { width: 1400, height: 900 },
  recordVideo: { dir: outDir, size: { width: 1400, height: 900 } },
});
const page = await context.newPage();
await page.goto(url, { waitUntil: 'load' });

const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });
console.log('graphical mode reached, capturing for ' + durationMs + 'ms (a full idle-tour lap: 2 multi-window rounds + 5 solo apps + the satellite wallpaper demo)...');

const t0 = Date.now();
let shot = 0;
while (Date.now() - t0 < durationMs) {
  const elapsed = Date.now() - t0;
  await page.screenshot({ path: outDir + '/shot-' + String(shot).padStart(3, '0') + '-t' + elapsed + 'ms.png' });
  shot++;
  await page.waitForTimeout(4000);
}

await context.close(); // flushes the real .webm video file into outDir
await browser.close();
console.log('done: ' + shot + ' screenshots + 1 video written to ' + outDir);
