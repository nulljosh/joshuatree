// MANUAL: needs Playwright's Chromium browser binaries plus a local dev server at localhost:3000 (landing/), neither installed in CI (.github/workflows/check.yml has no npm/playwright step). Run by hand after npm install && npx playwright install.
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
// v62: also checks the absolute-pointer path. The page exposes whether the
// kernel enabled v86's VMware absolute-mouse backdoor (`__jt.absolute`) and
// the kernel's own serial log (`__jt.serial`), so this can assert that the
// GUEST saw an absolute packet, not just that the page sent one, and it
// times how long a dock tap takes to open an app: the whole point of the
// change is that the pointer no longer walks there first.
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
const state = () => page.evaluate(() => window.__jt ? { ready: __jt.ready, focused: __jt.focused, mouseOn: __jt.mouseOn, absolute: __jt.absolute } : 'no hook');
const serialLines = (needle) => page.evaluate(n => (window.__jt && __jt.serial || '').split('\n').filter(l => l.includes(n)), needle);

console.log('input state:', JSON.stringify(await state()));
console.log('kernel serial (vmmouse):', JSON.stringify(await serialLines('vmmouse')));

// Tap once to focus/enable input (the embed gates input behind a real
// tap), then tap a dock icon. The focus tap also stops the idle tour and,
// if the tour already had an app open (it opens Files ~6.5s after
// graphical mode, i.e. before this script's own settle wait is over),
// closes it, since a click anywhere closes an app view. So `before` is
// sampled AFTER this tap, on the settled desktop: v62 caught the earlier
// version of this script sampling `before` with the tour's Files view
// still open, which made "the app closed" indistinguishable from "an app
// opened" and passed for the wrong reason.
const box = await canvas.boundingBox();
await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
const absolute = (await state()).absolute === true;
// Relative fallback: let the paced homing+travel finish before the next
// tap. Absolute: the pointer is already there, a beat is plenty.
await page.waitForTimeout(absolute ? 1500 : 5000);
console.log('before tap:', await shot('1-desktop'));
const before = await fingerprint();
console.log('  fingerprint', JSON.stringify(before));

// Dock geometry in LOGICAL kernel pixels, the same arithmetic as
// kernel.c's gui_dock_icon/gui_dock_x0/gui_slot_x (v59: 10 slots; v52:
// default dock_scale_pct 7, so a tile is floor(540*7/100) = 37px, well
// under the DOCK_BUDGET width cap). Slot 1 is Files (slot 0 is the Apps
// folder). A fresh v86 boot has no SETTINGS.TXT, so the default scale is
// what's really on screen.
const KW = 960, KH = 540;
const ICON = Math.floor(KH * 7 / 100), GAP = 6, PAD = 10, COUNT = 10, MARGIN_BOT = 24, SLOT = 1;
const dockW = COUNT * ICON + (COUNT - 1) * GAP + 2 * PAD;
const dockX0 = Math.floor((KW - dockW) / 2);
const iconCX = dockX0 + PAD + SLOT * (ICON + GAP) + ICON / 2;
const iconCY = KH - ICON - 2 * PAD - MARGIN_BOT + PAD + ICON / 2;

const sx = box.x + (iconCX / KW) * box.width;
const sy = box.y + (iconCY / KH) * box.height;
console.log(`tapping dock slot ${SLOT} at kernel (${iconCX.toFixed(0)},${iconCY.toFixed(0)}) -> screen (${sx.toFixed(0)},${sy.toFixed(0)}), absolute=${absolute}`);

const t0 = Date.now();
await page.touchscreen.tap(sx, sy);
// Poll instead of sleeping a fixed 9s: the time until the app view shows
// up is the real number this change is about.
let after = before, openedAt = -1;
const looksOpened = (a, b) => a.light > b.light * 2 + 50 || a.light < b.light / 2;
while (Date.now() - t0 < 9000) {
  await page.waitForTimeout(100);
  after = await fingerprint();
  if (looksOpened(after, before)) { openedAt = Date.now() - t0; break; }
}
// A transient (one mid-redraw frame) must not count: the app view has to
// still be there a second and a half later.
if (openedAt >= 0) {
  await page.waitForTimeout(1500);
  const settled = await fingerprint();
  if (!looksOpened(settled, before)) { console.log('  transient only: screen went back to', JSON.stringify(settled)); openedAt = -1; }
  after = settled;
}
console.log('input state after taps:', JSON.stringify(await state()));
console.log('kernel serial (vmmouse):', JSON.stringify(await serialLines('vmmouse')));

console.log('after tap:', await shot('2-after-tap'));
console.log('  fingerprint', JSON.stringify(after));

// v67 (0.62.2): the "stuck on Notes" report, on the real v86 path. Notes
// ran its own relative-only mouse loop and never saw v86's absolute
// pointer, so no tap could ever reach its close hitbox, and with the dock
// still visible around the modal window every dock tap after it looked
// dead ("all apps"). Close whatever the first tap opened (a tap anywhere
// closes it), open Notes (dock slot 4), type real text through v86's own
// keyboard_send_text (so the disk-less save path runs on close too, this
// boot has no FAT), then tap the window's own red close button at kernel
// (94,56): the desktop has to come back.
let notesClosed = null;
if (openedAt >= 0) {
  await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
  await page.waitForTimeout(1500);
  const desk = await fingerprint();
  const NOTES_SLOT = 4;
  const notesCX = dockX0 + PAD + NOTES_SLOT * (ICON + GAP) + ICON / 2;
  await page.touchscreen.tap(box.x + (notesCX / KW) * box.width, sy);
  await page.waitForTimeout(1500);
  const notes = await fingerprint();
  const notesOpened = looksOpened(notes, desk);
  await page.evaluate(() => { if (window.__jt && __jt.emu.keyboard_send_text) __jt.emu.keyboard_send_text('hello', 40); });
  await page.waitForTimeout(900);
  await shot('3-notes-open');
  await page.touchscreen.tap(box.x + (94 / KW) * box.width, box.y + (56 / KH) * box.height);
  await page.waitForTimeout(1500);
  const afterX = await fingerprint();
  notesClosed = notesOpened && !looksOpened(afterX, desk);
  await shot('4-notes-closed');
  console.log(`notes: opened=${notesOpened} (light ${desk.light} -> ${notes.light}), closed by its X=${notesClosed} (light ${afterX.light})`);
}

// An app view clears the whole screen to one flat colour, so a real
// launch shows up as a large swing in how many sampled pixels are light,
// in EITHER direction: most apps clear to near-white, but Terminal
// clears to near-black, and an earlier version of this check only looked
// for "got brighter" and therefore reported a working Terminal launch as
// a failure. The desktop photo never looks like either extreme.
const changed = Math.abs(after.sum - before.sum) > before.sum * 0.05;
const opened = looksOpened(after, before);
const guestSawAbsolute = (await serialLines('vmmouse: first absolute packet')).length > 0;
console.log('\nRESULT');
console.log('  screen changed at all :', changed);
console.log('  looks like an app view:', opened, `(light px ${before.light} -> ${after.light})`);
console.log('  tap to app view       :', openedAt >= 0 ? openedAt + 'ms' : 'never');
console.log('  guest absolute mode   :', absolute, guestSawAbsolute ? '(kernel logged a real absolute packet)' : '(kernel never logged an absolute packet)');
console.log('  notes opens and closes:', notesClosed === null ? 'not reached' : notesClosed);
if (logs.length) console.log('  console:', logs.slice(-8).join(' | '));
if (process.env.JT_SERIAL) { console.log('--- full kernel serial log'); console.log(await page.evaluate(() => window.__jt ? __jt.serial : '')); }
const pass = opened && (!absolute || guestSawAbsolute) && notesClosed === true;
console.log(pass ? '\nPASS: a tap on the dock opened an app, and Notes closed from its own X' : '\nFAIL: ' + (opened ? 'Notes did not open and close by tap' : 'tapping the dock did not open an app'));

await browser.close();
process.exit(pass ? 0 : 1);
