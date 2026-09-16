#!/usr/bin/env node
// Real discriminating test for wheel scrolling in the Apps folder.
// Opens the landing page, enters the Apps folder, and verifies that wheel
// events actually scroll the app grid (would fail without the browser-side
// forwarding fix in embed.js).
//
// Usage: node tools/checks/appsfolder-wheelscroll-test.mjs [url]
import { chromium, devices } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';

const browser = await chromium.launch();
const context = await browser.newContext();
const page = await context.newPage();

console.log('loading', url);
await page.goto(url, { waitUntil: 'load' });

// Wait for the canvas to be fully rendered
const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });
console.log('canvas is up:', await canvas.evaluate(c => c.width + 'x' + c.height));

// Wait for GUI boot + initial desktop paint
await page.waitForTimeout(9000);

// Get a fingerprint of the canvas for comparison
async function fingerprint() {
  return await page.evaluate(() => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let sum = 0;
    for (let i = 0; i < d.length; i += 4 * 97) {
      sum += d[i] + d[i + 1] + d[i + 2];
    }
    return sum;
  });
}

// Take initial screenshot
await page.screenshot({ path: '/tmp/appsfolder-wheel-1-before.png' });
const before = await fingerprint();

// Get the canvas bounding box for event sending
const box = await canvas.boundingBox();
const canvasX = box.x + box.width / 2;
const canvasY = box.y + box.height / 2;

// Focus the emulator by clicking on the canvas
console.log('focusing emulator...');
await page.mouse.click(canvasX, canvasY);
await page.waitForTimeout(500);

// Navigate to Apps folder: press 'a' to open Apps folder launchpad
console.log('opening Apps folder...');
await page.keyboard.press('a');
await page.waitForTimeout(1500);

// Take screenshot of the Apps folder
await page.screenshot({ path: '/tmp/appsfolder-wheel-2-apps-before-scroll.png' });
const appsBefore = await fingerprint();
console.log('fingerprint before wheel scroll:', appsBefore);

// Send wheel down event (multiple times to ensure visible scroll)
console.log('sending wheel scroll down events...');
for (let i = 0; i < 3; i++) {
  // v86's wheel API expects [delta, 0] where negative = scroll down
  // We use the browser's wheel event which normalizes to the same
  await page.evaluate(() => {
    const ev = new WheelEvent('wheel', {
      bubbles: true,
      cancelable: true,
      deltaY: 100, // positive = scroll down
      clientX: window.innerWidth / 2,
      clientY: window.innerHeight / 2
    });
    document.getElementById('screen_container').dispatchEvent(ev);
  });
  await page.waitForTimeout(100);
}

await page.waitForTimeout(500);

// Take screenshot after scrolling
await page.screenshot({ path: '/tmp/appsfolder-wheel-3-apps-after-scroll.png' });
const appsAfter = await fingerprint();
console.log('fingerprint after wheel scroll:', appsAfter);

// Verify that the content changed (different icons are now visible)
// A significant scroll would show different app icons, changing the pixel pattern
const pixelDiff = Math.abs(appsAfter - appsBefore);
const percentChange = (pixelDiff / appsBefore) * 100;
console.log(`pixel difference: ${pixelDiff} (${percentChange.toFixed(1)}%)`);

// If wheel scrolling works, we should see a visible change in the app grid
// A scroll of 3 rows of icons should be at least 5% pixel difference
const scrollWorked = percentChange > 5;

console.log('\nRESULT');
console.log('  fingerprint before:', appsBefore);
console.log('  fingerprint after:', appsAfter);
console.log('  pixel difference:', pixelDiff, `(${percentChange.toFixed(1)}%)`);
console.log('  wheel scroll worked:', scrollWorked);

if (scrollWorked) {
  console.log('\nPASS: wheel scrolling in Apps folder works (pixel content changed)');
} else {
  console.log('\nFAIL: wheel scrolling had no visible effect (pixel content unchanged)');
}

await browser.close();
process.exit(scrollWorked ? 0 : 1);
