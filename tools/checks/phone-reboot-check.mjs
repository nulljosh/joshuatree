// MANUAL: about 2.5 minutes of real wall-clock (one full tour lap), too slow for every push. Proves a phone visitor stays in
// phone mode after the tour's lap-boundary reboot. Before 1.7.16, reinjectKernel
// reloaded the kernel with no cmdline, so the live site booted at 860x1520 and came
// back as a letterboxed 1920x1080 desktop (and lost facehost) after lap 1.
// Usage: node tools/checks/phone-reboot-check.mjs [url]   (default: the live site)
import { chromium, devices } from 'playwright';
const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com/';
const b = await chromium.launch();
const p = await (await b.newContext({ ...devices['iPhone 15'] })).newPage();
await p.goto(url, { waitUntil: 'load' });
await p.locator('#stage-wrap').scrollIntoViewIfNeeded();
const dims = () => p.evaluate(() => { const c = document.querySelector('#screen_canvas') || document.querySelector('#stage-wrap canvas'); return c ? c.width + 'x' + c.height : 'none'; });
await p.evaluate(() => { window.__laps = 0; window.addEventListener('jt-tour-lap-done', () => window.__laps++); });
await p.waitForTimeout(20000);
const before = await dims();
const t0 = Date.now();
while (Date.now() - t0 < 300000) { await p.waitForTimeout(10000); if (await p.evaluate(() => window.__laps) > 0) break; }
const laps = await p.evaluate(() => window.__laps);
await p.waitForTimeout(25000);
const after = await dims();
await b.close();
console.log(`boot ${before}, laps ${laps}, after reboot ${after}`);
if (laps < 1) { console.log('FAIL: the tour never finished a lap'); process.exit(1); }
if (before !== '860x1520' || after !== before) { console.log('FAIL: phone mode did not survive the lap reboot'); process.exit(1); }
console.log('PASS: phone mode survives the tour-lap reboot');
