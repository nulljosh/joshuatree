// MANUAL: needs Playwright's Chromium plus the built landing/v86/kernel.elf (make kernel.elf copies it there). Hosts landing/ on its own throwaway local port; not in CI.
// Regression test for the v71 idle-tour bug, updated for the 2.0 lap:
// keyboard sends the tour makes while nobody has focused the demo were
// silently swallowed by libv86.js's keyboard_adapter.emu_enabled gate, so
// the tour's typed interactions never landed even though the sends
// themselves completed with no error. The tour's loop sets
// keyboard_adapter.emu_enabled = true next to the mouse_adapter line.
//
// This test never taps or focuses the page (that is the real-visitor path,
// never broken, and not what this checks): it watches the real, unmodified
// idle tour and reads the guest's own serial log, the one place a landed
// keystroke or a real window shows up without guessing at pixels. The 2.0
// lap opens with Mail composing inline, then Burrow:
//   mail: ring-3 window        Mail's window opened from its dock tile
//   mail: compose=1            the scripted 'c' reached the guest
//   mail: filed n=3            all three typed fields landed and Enter filed a 3rd message
//   mail: closed               Escape closed Mail (no Mail left behind Burrow)
//   burrow: ring-3 window      Burrow opened next, after Mail was gone
//   burrow: saved view=0       its scripted '2' then '1' keys reached the guest
//
// Usage: node tools/checks/tourinput-check.mjs   (JT_CHROMIUM overrides the browser)
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
if (!fs.existsSync(path.join(root, 'v86', 'kernel.elf'))) {
  console.error('FAIL: landing/v86/kernel.elf missing, run `make kernel.elf` first');
  process.exit(1);
}
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.webp': 'image/webp' };
const server = http.createServer((q, r) => {
  const u = q.url.split('?')[0];
  const f = path.join(root, u === '/' ? 'index.html' : decodeURIComponent(u));
  if (!f.startsWith(root) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const url = process.argv[2] || `http://localhost:${server.address().port}/index.html`;

const CHROMIUM_PATH = process.env.JT_CHROMIUM || (fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined);
const browser = await chromium.launch(CHROMIUM_PATH ? { executablePath: CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.route('**/api/proxy**', r => r.fulfill({ status: 403, body: '' })); // hermetic: nothing here needs the network
await page.goto(url, { waitUntil: 'load' });

// In order, each one only counts after the one before it (Mail must be gone
// before Burrow opens, so a Mail left behind fails here).
const STEPS = [
  ['mail: ring-3 window', 'Mail opened'],
  ['mail: compose=1', 'the scripted c reached Mail'],
  ['mail: filed n=3', 'the three typed fields landed and filed a 3rd message'],
  ['mail: closed', 'Escape closed Mail'],
  ['burrow: ring-3 window', 'Burrow opened after Mail closed'],
  ['burrow: saved view=0', "Burrow's scripted 2 then 1 keys landed"],
];
let pass = false, seenAt = -1, step = 0;
const t0 = Date.now();
try {
  await page.waitForFunction(() => window.__jt && window.__jt.serial !== undefined, null, { timeout: 60000 });
  while (Date.now() - t0 < 90000 && step < STEPS.length) {
    await page.waitForTimeout(400);
    const log = await page.evaluate(() => window.__jt.serial);
    while (step < STEPS.length) {
      const at = log.indexOf(STEPS[step][0], seenAt + 1);
      if (at < 0) break;
      seenAt = at;
      console.log('  ok:   t=' + ((Date.now() - t0) / 1000).toFixed(1) + 's ' + STEPS[step][1] + ' (' + STEPS[step][0] + ')');
      step++;
    }
  }
  pass = step === STEPS.length;
  if (!pass) { const lg = await page.evaluate(() => window.__jt.serial); console.log('DEBUG len=' + lg.length + ' ' + JSON.stringify(lg.split('\n').filter(l => /mail:|burrow:/.test(l)))); }
  if (!pass) console.log('  FAIL: never saw "' + STEPS[step][0] + '" (' + STEPS[step][1] + ') in order within 90s, the tour may be stuck or its keys are being swallowed');
} finally {
  await browser.close();
  server.close();
}
console.log(pass
  ? '\nPASS: the idle tour\'s own scripted Mail compose and Burrow keys landed in the guest with no visitor interaction'
  : '\nFAIL: the idle tour did not complete its Mail then Burrow opening (keyboard_adapter.emu_enabled regression or tour stall)');
process.exit(pass ? 0 : 1);
