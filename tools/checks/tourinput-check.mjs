// Regression test for the v71 idle-tour bug: keyboard_send_text calls the
// tour makes while nobody has focused the demo were silently swallowed by
// libv86.js's own keyboard_adapter.emu_enabled gate (embed.js sets it
// false on "emulator-ready" and only ever true inside focusIn(), so the
// tour's typed interactions into Mail/Calendar/Notes/Reminders/Terminal/
// Chat never landed, even though the sends themselves completed with no
// error). Fixed by also setting emulator.keyboard_adapter.emu_enabled =
// true alongside the existing mouse_adapter line in the tour's own loop.
//
// This test never taps or focuses the page at all (that would take the
// real-visitor path, which was never broken and isn't what this checks):
// it watches the real, unmodified idle tour open Mail on its own and
// asserts the on-screen message list actually grows from 2 seed messages
// to 3 once the scripted compose (`c` + three typed fields) has had time
// to run, the one real, visible signature only a landed keystroke can
// produce (Mail's own list count, not just "the screen changed color").
//
// Usage: node tools/checks/tourinput-check.mjs [url]
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

// The tour arms 6s after graphical mode (already reached), opens Files
// (dwell ~15s, real content already bright/white like Mail's own list),
// closes to the dark desktop wallpaper, THEN opens Mail. Files and Mail
// can't be told apart by brightness alone, so this walks a real 3-state
// machine (bright -> dark -> bright) rather than reacting to the first
// swing, the same real transition sequence tourwatch-qa.mjs (this pass's
// own throwaway watcher) observed live against both the old and new tour.
const DARK_MAX = 8000000, BRIGHT_MIN = 12000000;
let state = 'unknown', brightCount = 0, mailOpenedAt = -1;
const t0 = Date.now();
while (Date.now() - t0 < 60000 && mailOpenedAt < 0) {
  await page.waitForTimeout(500);
  const fp = await fingerprint();
  if (fp < DARK_MAX && state !== 'dark') { state = 'dark'; }
  else if (fp > BRIGHT_MIN && state !== 'bright') {
    state = 'bright';
    brightCount++;
    if (brightCount === 2) mailOpenedAt = Date.now(); // 1st bright = Files, 2nd = Mail
  }
}
if (mailOpenedAt < 0) {
  console.log('FAIL: never saw the real Files-open, close, Mail-open sequence within 60s, the tour may be stuck');
  await browser.close();
  process.exit(1);
}
console.log('Mail opened (2nd bright phase) at t=' + ((mailOpenedAt - t0) / 1000).toFixed(1) + 's, waiting for its scripted compose to finish...');

// Give the compose script (c + 3 Enter-confirmed fields, ~5.5s of real
// typing plus settle/wait steps) the rest of the ~15s dwell to land.
await page.waitForTimeout(11000);

const text = await page.evaluate(() => {
  // No OCR here: sample the row where a 3rd list entry would sit. Mail's
  // list draws at VIEWPORT-LOCAL coordinates (y=84+2*22=128 for row 2,
  // x=60), not full-screen ones: gui_launch_from_dock's non-apps-folder
  // window is x=70,y=40,w=820,h=385 and clips app drawing into
  // window_set_viewport(x+8, y+32, ...), so the real full-LOGICAL-screen
  // position is (70+8+60, 40+32+128) = (138, 200). Compare that band
  // against the row above it (row 1, always a real seed message, "About
  // this app"), a real, always-present control for what "real text
  // pixels" looks like in this same font/window at this same scale.
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
  return { row1: darkPixelsAt(178), row2: darkPixelsAt(200) }; // row1 y=40+32+106=178, row2 y=40+32+128=200
});
console.log('dark (text) pixels sampled: row1 (known real seed message)=' + text.row1 + ', row2 (only real if compose landed)=' + text.row2);

// row2 needs real text-scale dark-pixel density, not just "greater than
// zero" (compare it against row1, which is always a real message, rather
// than a fixed constant, so this stays correct if the font or theme ever
// changes): a blank row reads near 0 regardless of theme.
const pass = text.row2 > text.row1 * 0.3;
console.log(pass
  ? '\nPASS: the idle tour\'s own scripted Mail compose landed a real 3rd message with no visitor interaction'
  : '\nFAIL: the idle tour opened Mail but its scripted keys never landed (keyboard_adapter.emu_enabled regression)');
await browser.close();
process.exit(pass ? 0 : 1);
