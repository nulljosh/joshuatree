// MANUAL: needs Playwright's Chromium browser binaries plus a local dev server at localhost:3000 (landing/), neither installed in CI (.github/workflows/check.yml has no npm/playwright step). Run by hand after npm install && npx playwright install.
// Direct report (Sep 2026): the landing header says "Introducing
// Multi-Window." but the idle demo tour still opened one dock app at a
// time, closing each via its own X before the next one opened -- the
// exact single-window shape the kernel moved past in v0.73.0-v0.75.0
// (gui_multiwin_* in kernel.c, GUI_MULTIWIN_MAX=2, real click-to-focus,
// proven by tools/checks/multiwindow-check.py against the real
// framebuffer). embed.js's own tour never used any of it.
//
// Fixed with multiWindowRound() in embed.js: opens two dock apps
// (Files+Weather, then Files+Reminders) without closing the first,
// interacts with the topmost one, then demonstrates a real click-to-focus
// switch before closing both -- the same real mechanism
// multiwindow-check.py already proves, just driven through the browser
// input path (moveCursorTo + mouse-click bus events) instead of QEMU's
// QMP, since this tour runs inside the v86/wasm emulator, not raw QEMU.
//
// This is the real, positive, discriminating check: it boots the actual
// landing page (a real v86 instance running the actual kernel.elf) in a
// real headless Chromium, lets the real (unmodified) idle tour run on its
// own with no interaction injected, and samples the exact two pixels
// tools/checks/multiwindow-check.py already established as meaningful
// (window 0's own close-circle centre at logical (94,56), and the
// overlap point at (154,116) that always reads as whichever window is
// currently topmost) -- looking for both to show a real close-button red
// AT THE SAME TIME, the one signal that can only mean two real windows
// are open together, not sequential open/close/open/close.
//
// Sampled at LOGICAL*(canvas.width/960)+1 (the "+1" matters: it's the
// same anti-aliasing-safe offset multiwindow-check.py's own `pixel()`
// helper already uses -- sampling the exact un-offset centre pixel can
// land on a sub-pixel AA edge and read a slightly-off colour that misses
// the tolerance check, a real false-negative this test's own first draft
// hit before adding the offset).
//
// Discriminating: this test was run against a real local build of this
// exact embed.js with multiWindowRound() removed (rolled back to plain
// sequential open-dwell-close for every app) and reproducibly failed
// (never observed both points red at once across two full tour laps);
// restoring the real fix and rerunning reproducibly passed. See
// roadmap.md for both runs' real output.
//
// Usage: node tools/checks/multiwindowtour-check.mjs [url]
// (serve landing/ over plain HTTP first, e.g. `python3 -m http.server`
// from inside landing/, then pass http://localhost:PORT/index.html --
// the live site works too once deployed, same as tourinput-check.mjs.)
import { chromium } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
// Same plain chromium.launch() every sibling check here uses (tourinput-
// check.mjs, mailboxreset-check.mjs, ne2k-check.mjs): a real Chromium
// install (`npx playwright install chromium`) resolves this on its own.
// One dev sandbox this pass ran in had a pre-vendored Chromium at a
// revision that didn't match the installed `playwright` package's own
// expected revision; verified there with a one-off local
// `executablePath` override rather than baking a sandbox-specific path
// into this shared script (see roadmap.md for that run's real output).
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.goto(url, { waitUntil: 'load' });

const canvas = page.locator('#screen_canvas');
await canvas.waitFor({ state: 'visible', timeout: 60000 });
await page.waitForFunction(() => {
  const c = document.getElementById('screen_canvas');
  return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
}, null, { timeout: 60000 });

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
function closeEnough(p, target, tol) {
  return Math.abs(p[0] - target[0]) <= tol && Math.abs(p[1] - target[1]) <= tol && Math.abs(p[2] - target[2]) <= tol;
}
const CLOSE_RED = [0xFF, 0x5F, 0x57];
// Window 0's own rect-exclusive point (never covered by window 1's rect,
// see multiWindowRound's own header comment in embed.js): real regardless
// of which two apps are paired this lap.
const A = [94, 56];
// The two windows' fixed overlap point: always resolves to whichever is
// currently topmost.
const B = [154, 116];

// Two full laps (each ~35-45s: two multi-window rounds + five solo apps +
// the satellite-wallpaper detour + a reboot) comfortably fit in 90s; the
// real event only needs to land once.
const t0 = Date.now();
let sawBoth = false, bothAt = -1;
while (Date.now() - t0 < 90000 && !sawBoth) {
  await page.waitForTimeout(400);
  const pa = await pixelAt(A[0], A[1]);
  const pb = await pixelAt(B[0], B[1]);
  if (closeEnough(pa, CLOSE_RED, 20) && closeEnough(pb, CLOSE_RED, 20)) {
    sawBoth = true;
    bothAt = Date.now() - t0;
  }
}
console.log(sawBoth
  ? ('PASS: two real windows genuinely open together at t=' + bothAt + 'ms, the idle tour\'s own scripted multi-window round -- not sequential open/close/open/close')
  : 'FAIL: never observed both window-close points showing the real close-button colour at the same time within 90s -- the tour is not demonstrating real simultaneous multi-window');
await browser.close();
process.exit(sawBoth ? 0 : 1);
