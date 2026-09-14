// Real mobile QA for the live landing-page demo, using a real iPhone
// emulation (real touch events, real iOS user agent, real viewport), not a
// desktop browser with a narrow window. This exists because "apps don't
// open on mobile" was reported three separate times and each attempted fix
// was verified only by reasoning about the code, which is exactly how a
// bug survives three fixes.
//
// It drives the REAL deployed site by default so it tests what a visitor
// actually gets, including the deployed kernel.elf, not a local build that
// may differ. Pass a URL to test somewhere else.
//
// Usage: node mobiletest.mjs [url]
import { chromium, devices } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
const iPhone = devices['iPhone 13'];

const browser = await chromium.launch();
const context = await browser.newContext({ ...iPhone });
const page = await context.newPage();

const logs = [];
page.on('console', m => logs.push(m.text()));
page.on('pageerror', e => logs.push('PAGEERROR: ' + e.message));

console.log('loading', url, 'as', iPhone.userAgent.slice(0, 40) + '...');
await page.goto(url, { waitUntil: 'load' });

// The kernel has to actually boot into graphical mode before any of this
// means anything. Poll the real canvas rather than sleeping a guessed
// amount: v86's boot time varies a lot between machines.
const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });
console.log('canvas is up:', await canvas.evaluate(c => c.width + 'x' + c.height));

// Give the GUI a moment to actually paint the desktop after mode-set.
await page.waitForTimeout(9000);

async function shot(name) {
  await page.screenshot({ path: `/tmp/mobiletest-${name}.png` });
  return `/tmp/mobiletest-${name}.png`;
}

// Sample the real canvas pixels. Comparing before/after a tap is the only
// honest way to know whether anything happened: the page can't ask the
// kernel what it's showing.
async function fingerprint() {
  return await page.evaluate(() => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let sum = 0, light = 0;
    for (let i = 0; i < d.length; i += 4 * 97) {
      sum += d[i] + d[i + 1] + d[i + 2];
      if (d[i] > 200 && d[i + 1] > 200 && d[i + 2] > 200) light++;
    }
    return { sum, light };
  });
}

console.log('input state:', JSON.stringify(await page.evaluate(() => window.__jt ? {ready:__jt.ready, focused:__jt.focused, mouseOn:__jt.mouseOn} : 'no hook')));
console.log('before tap:', await shot('1-desktop'));
const before = await fingerprint();
console.log('  fingerprint', JSON.stringify(before));

// Tap once to focus/enable input (the embed gates input behind a real
// tap), then tap a dock icon.
const box = await canvas.boundingBox();
await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
await page.waitForTimeout(5000); // let the paced homing+travel finish before the next tap

// Dock icons sit along the bottom of the kernel's own 800x600 output,
// centred. Icon 0 (Terminal) is the leftmost of 7. Compute in kernel
// coordinates, then map into the canvas's real on-screen box.
const KW = 800, KH = 600;
const ICON = 60, GAP = 6, PAD = 10, COUNT = 7, MARGIN_BOT = 24;
const dockW = COUNT * ICON + (COUNT - 1) * GAP + 2 * PAD;
const dockX0 = (KW - dockW) / 2;
const iconCX = dockX0 + PAD + ICON / 2;          // centre of icon 0
const iconCY = KH - MARGIN_BOT - PAD - ICON / 2; // vertical centre of the icon row

const sx = box.x + (iconCX / KW) * box.width;
const sy = box.y + (iconCY / KH) * box.height;
console.log(`tapping dock icon 0 at kernel (${iconCX.toFixed(0)},${iconCY.toFixed(0)}) -> screen (${sx.toFixed(0)},${sy.toFixed(0)})`);

await page.touchscreen.tap(sx, sy);
await page.waitForTimeout(9000);
console.log('input state after taps:', JSON.stringify(await page.evaluate(() => window.__jt ? {ready:__jt.ready, focused:__jt.focused, mouseOn:__jt.mouseOn} : 'no hook')));

console.log('after tap:', await shot('2-after-tap'));
const after = await fingerprint();
console.log('  fingerprint', JSON.stringify(after));

// An app view clears the whole screen to one flat colour, so a real
// launch shows up as a large swing in how many sampled pixels are light,
// in EITHER direction: most apps clear to near-white, but Terminal
// clears to near-black, and an earlier version of this check only looked
// for "got brighter" and therefore reported a working Terminal launch as
// a failure. The desktop photo never looks like either extreme.
const changed = Math.abs(after.sum - before.sum) > before.sum * 0.05;
const opened = after.light > before.light * 2 + 50 || after.light < before.light / 2;
console.log('\nRESULT');
console.log('  screen changed at all :', changed);
console.log('  looks like an app view:', opened, `(light px ${before.light} -> ${after.light})`);
if (logs.length) console.log('  console:', logs.slice(-8).join(' | '));
console.log(opened ? '\nPASS: a tap on the dock opened an app' : '\nFAIL: tapping the dock did not open an app');

await browser.close();
process.exit(opened ? 0 : 1);
