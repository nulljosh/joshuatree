// MANUAL: needs Playwright's Chromium browser binaries plus a local dev server at localhost:3000 (landing/), neither installed in CI (.github/workflows/check.yml has no npm/playwright step). Run by hand after npm install && npx playwright install.
// The v86 half of wallboot-check.sh's guarantee: on the browser demo
// (font_is_fallback()==1, no real network ever, see wall_apply()'s own
// comment in kernel/kernel.c), the desktop's automatic no-network
// wallpaper fallback must be the baked SATELLITE capture
// (kernel/wall_sat.h, tools/gen/gen_wall_sat.py), never the tree photo
// (wallpaper_rgb). Direct owner request, Sep 2026: "the demo should show
// the satellite wallpaper only, no tree wallpaper anymore" -- wallboot-
// check.sh already proves the native-QEMU half (first paint is the dark
// fallback, not the tree, while a real fetch is pending); this proves the
// v86 half, where there is no pending-fetch window at all, the satellite
// fallback is permanent for the whole session.
//
// Boots the real kernel inside the real v86/libv86.js emulator (the same
// engine landing/index.html embeds), headless, via Playwright, against a
// SCRATCH COPY of landing/ with this build's own fresh kernel.elf dropped
// in -- never touches the real landing/ files or the committed
// landing/v86/kernel.elf, per this repo's own rule about not touching
// landing/ mid-session. Reads window.__jt.serial (the kernel's own serial
// log, exposed for exactly this kind of real-evidence test -- see
// tools/checks/ne2k-check.mjs, tools/checks/mobiletest.mjs) for the
// `wallsrc=` marker wall_apply() logs at its one real choke point.
//
// Proven discriminating, not just "prints ok": temporarily commenting out
// the `else if (want_map && !wall_map && font_is_fallback())` branch this
// change added to wall_apply() (kernel/kernel.c) reverts to the old
// v0.79.2 behavior (tree photo, permanently, in v86) and makes this fail
// with `wallsrc=photo` as the only line -- verified by hand during this
// check's own authorship, restored immediately after; see the commit that
// added this file for the real before/after serial logs.
//
// Usage: node tools/checks/wallboot-v86-check.mjs
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { mkdtempSync, cpSync, copyFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
const PORT = 8935;

function run(cmd, args, opts) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { cwd: ROOT, stdio: 'inherit', ...opts });
    p.on('exit', (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} exited ${code}`))));
  });
}

console.log('building kernel.elf...');
await run('make', ['-s', 'kernel.elf']);

const scratch = mkdtempSync(join(tmpdir(), 'jt-wallboot-v86-'));
cpSync(join(ROOT, 'landing'), scratch, { recursive: true });
copyFileSync(join(ROOT, 'kernel.elf'), join(scratch, 'v86', 'kernel.elf'));
console.log('scratch copy of landing/ at', scratch, 'with this build\'s fresh kernel.elf');

const server = spawn('python3', ['-m', 'http.server', String(PORT)], { cwd: scratch, stdio: 'ignore' });
await new Promise((r) => setTimeout(r, 500));

let exitCode = 1;
try {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
  await page.goto(`http://localhost:${PORT}/index.html`, { waitUntil: 'load' });

  // The idle timer's own click-to-focus dwell isn't needed: the kernel
  // boots and paints the desktop with no interaction at all, the same
  // reason ne2k-check.mjs and live-wallpaper-check.mjs don't click
  // anything before reading serial output either.
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  console.log('v86 adapters ready');

  const deadline = Date.now() + 20000;
  let serial = '';
  while (Date.now() < deadline) {
    serial = await page.evaluate(() => (window.__jt && window.__jt.serial) || '');
    if (/wallsrc=/.test(serial)) break;
    await page.waitForTimeout(500);
  }

  await browser.close();

  console.log('--- kernel serial log (wallsrc= lines) ---');
  const wallsrcLines = serial.match(/wallsrc=\S+/g) || [];
  for (const l of wallsrcLines) console.log(l);
  console.log('--- end ---');

  const sawPhoto = wallsrcLines.includes('wallsrc=photo');
  const sawSatFallback = wallsrcLines.includes('wallsrc=satfallback');

  if (wallsrcLines.length === 0) {
    console.log('FAIL: no wallsrc= line ever appeared on serial within budget');
  } else if (sawPhoto) {
    console.log('FAIL: wallsrc=photo appeared -- the tree photo showed in the v86 demo, the exact thing this change removes');
  } else if (!sawSatFallback) {
    console.log('FAIL: no wallsrc=satfallback line -- the baked satellite capture never became the wallpaper source');
  } else {
    console.log('PASS: the v86 demo\'s automatic no-network wallpaper is the baked satellite capture, never the tree photo');
    exitCode = 0;
  }
} finally {
  server.kill();
  rmSync(scratch, { recursive: true, force: true });
}

process.exit(exitCode);
