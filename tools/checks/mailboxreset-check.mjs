// Regression test for the real user-reported v0.72.3 bug: "every time it
// runs a demo and then restarts the demo, it adds like emails to the list,
// so there's duplicate emails." Root cause: this kernel's browser demo has
// no real disk (v0.71.0), so Mail/Reminders/etc. live in an in-memory
// ramfs, and the idle tour used to never reboot the emulator between full
// 8-app loops, it only closed and reopened windows. Every lap composed
// another real email into the SAME ramfs the last lap left behind, so the
// message count grew forever (2 seed -> 3 -> 4 -> ...). Fixed by a real
// emulator.restart() (libv86.js's Q.prototype.restart -> cpu.reboot_internal,
// a genuine CPU/kernel reboot) at the loop boundary, after all 8 apps have
// cycled, so ramfs starts fresh every lap.
//
// This test watches the real, unmodified idle tour run through TWO full
// 8-app laps with no interaction injected at all (same discipline as
// tourinput-check.mjs: this is the tour driving itself), samples Mail's
// on-screen message-list dark-pixel row count the first time Mail opens
// (lap 1) and again the second time Mail opens (lap 2), and asserts the
// two counts match. Before the fix this fails (lap 2's count is strictly
// greater, the duplicate-accumulation bug); after the fix it passes
// (lap 2 starts from the same fresh ramfs lap 1 did).
//
// Slow by nature: 2 real 8-app laps at DWELL_MS=7000 plus a real reboot
// between them is ~3 minutes of real wall-clock, not something to run on
// every push. Meant for manual verification of this exact fix, following
// the existing tools/checks/ naming convention, not wired into CI.
//
// Usage: node tools/checks/mailboxreset-check.mjs [url]
import { chromium } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
page.on('pageerror', function (e) { console.log('PAGE ERROR: ' + e); });
page.on('console', function (m) { if (m.type() === 'error') console.log('CONSOLE ERROR: ' + m.text()); });
await page.goto(url, { waitUntil: 'load' });

const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });

async function fingerprint() {
  return await page.evaluate(() => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let sum = 0;
    for (let i = 0; i < d.length; i += 4 * 61) sum += d[i] + d[i + 1] + d[i + 2];
    return sum;
  });
}

// The EXACT two coordinates tourinput-check.mjs already reverse-engineered
// and proved out against real Mail list pixels: row1 (y=178, list row
// index 1, "About this app", a real seed message present with or without
// any compose) as the always-real control, and row2 (y=200, list row
// index 2) which only carries real text once a scripted compose has
// actually landed and been sent. Only these two coordinates are proven;
// guessing further rows (224, 246, ...) risks sampling window chrome or
// blank canvas below the real list, not a longer list, so this test never
// extrapolates past what's actually been measured.
async function mailComposeLanded() {
  return await page.evaluate(() => {
    const c = document.getElementById('screen_canvas');
    const ctx = c.getContext('2d');
    const sx = c.width / 960, sy = c.height / 540;
    function darkPixelsAt(logicalY) {
      const rowY = Math.round(logicalY * sy), rowH = Math.round(16 * sy);
      const rowX0 = Math.round(138 * sx), rowW = Math.round(600 * sx);
      const d = ctx.getImageData(rowX0, rowY, rowW, rowH).data;
      let dark = 0;
      for (let i = 0; i < d.length; i += 4) if (d[i] < 150) dark++;
      return dark;
    }
    const row1 = darkPixelsAt(178);
    const row2 = darkPixelsAt(200);
    return { row1: row1, row2: row2, landed: row1 > 0 && row2 > row1 * 0.3 };
  });
}

// Walks the real bright/dark state machine tourinput-check.mjs already
// proved distinguishes Files (1st bright) from Mail (2nd bright) within a
// lap: Files -> dark desktop -> Mail -> dark desktop -> Calendar -> ...
// eight apps per lap, so the 2nd bright phase of each 8-phase block is
// always Mail. Runs across TWO full laps (16 bright phases) to catch both
// Mail opens, including the real reboot pause between laps 1 and 2, which
// shows up here as an extra-long dark stretch (the boot-logo/text-mode
// window) rather than a fixed sleep.
// DWELL_MS is only 7000 (v0.72.2 cut it from 15000 to keep the full 8-app
// loop under a minute), and Mail's own compose script takes ~6.8s of that
// to actually land its last Enter (the real send), leaving well under a
// second before the tour's own close click fires. A single fixed
// post-detection sleep (what tourinput-check.mjs uses, written back when
// DWELL_MS was 15000 and had 8s of slack) missed that landing entirely
// here, sampling 0 rows because it woke up after Mail had already closed.
// Fixed by sampling continuously through Mail's whole bright window and
// keeping the LAST reading seen before the screen goes dark again, so the
// count always reflects the state right before close, whatever the real
// margin, instead of guessing a wait long enough.
const DARK_MAX = 8000000, BRIGHT_MIN = 12000000;
let state = 'unknown', brightCount = 0;
const mailCounts = [];
const t0 = Date.now();
const OVERALL_TIMEOUT_MS = 200000; // ~3.3min budget for 2 real laps + a reboot, back down now the busy-loop overhead is gone
let lastDebugAt = 0;
while (Date.now() - t0 < OVERALL_TIMEOUT_MS && mailCounts.length < 2) {
  await page.waitForTimeout(250);
  if (Date.now() - lastDebugAt > 10000) {
    lastDebugAt = Date.now();
    console.log('  ...t=' + ((Date.now() - t0) / 1000).toFixed(0) + 's, brightCount=' + brightCount + ', state=' + state);
  }
  const fp = await fingerprint();
  if (fp < DARK_MAX && state !== 'dark') {
    state = 'dark';
  } else if (fp > BRIGHT_MIN && state !== 'bright') {
    state = 'bright';
    brightCount++;
    const posInLap = ((brightCount - 1) % 8) + 1; // 1=Files, 2=Mail, ...
    if (posInLap === 2) {
      console.log('Mail open #' + (mailCounts.length + 1) + ' detected at t=' + ((Date.now() - t0) / 1000).toFixed(1) + 's (bright phase ' + brightCount + ')');
      // v0.72.3 finding while building this test: polling getImageData in a
      // tight busy-loop here (the first version of this check) competes
      // hard with v86's own wasm CPU emulation for main-thread time in the
      // SAME renderer process, and visibly slows the tour's own real-time
      // setTimeout-driven pacing by 3x or more (confirmed: brightCount
      // barely advanced over minutes of wall clock with the busy-loop in
      // place). A real, not simulated, side effect of this measurement
      // technique, not a tour bug. Fixed by taking exactly ONE sample,
      // timed off the real compose script's own duration (~6.8s of the
      // 7000ms DWELL_MS) instead of polling for the close transition.
      await page.waitForTimeout(6400);
      var r = await mailComposeLanded();
      var count = r.row1 === 0 ? null : (r.landed ? 3 : 2); // 2 real seed messages always present, +1 once the scripted compose actually sent
      console.log('  row1=' + r.row1 + ' row2=' + r.row2 + ' compose landed=' + r.landed + ' -> message count=' + count);
      mailCounts.push(count);
    }
  }
}

await browser.close();

if (mailCounts.length < 2) {
  console.log('FAIL: never observed two full Mail opens (lap 1 and lap 2) within ' + (OVERALL_TIMEOUT_MS / 1000) + 's, the tour or the reboot wait may be stuck');
  process.exit(1);
}

const [lap1, lap2] = mailCounts;
console.log('\nlap 1 Mail message count: ' + lap1 + ', lap 2 Mail message count: ' + lap2);
const pass = lap1 !== null && lap2 !== null && lap2 === lap1;
console.log(pass
  ? 'PASS: ramfs reset between laps, lap 2 started from the same message count lap 1 did (real reboot, not accumulation)'
  : 'FAIL: message count changed between laps (' + lap1 + ' -> ' + lap2 + '), ramfs did not reset (the reported duplicate-email bug)');
process.exit(pass ? 0 : 1);
