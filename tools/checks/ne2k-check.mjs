// Real, headless proof that v86 (the landing page's in-browser x86
// emulator) can now reach a real network endpoint through the new NE2000
// driver (drivers/ne2k.c). v86 has never emulated RTL8139 (confirmed by
// grep, zero hits in landing/v86/libv86.js), which is why the wallpaper
// map feature (wall_fetch, a real geo lookup + tile fetch) always fell
// back to the static baked-in photo in the browser demo, even though the
// same feature works fine under real QEMU (tools/checks/geo-check.sh).
//
// Default wall_theme is WALL_WARM (a map theme, not WALL_PHOTO -- see
// kernel.c line ~790), so gui_run's own idle loop tries weather_fetch()
// (which calls net_init(), then geo_fetch()) and, if that lands, wall_fetch()
// automatically on the very first frame after the desktop draws, no
// keyboard/mouse interaction needed. weather_fetch mirrors its real
// results to the serial port: "geo=<lat>,<lon>", "wxurl=<path>",
// "wx=<condition>". This script reads exactly that log (the same real
// evidence geo-check.sh checks against native QEMU+RTL8139) out of the
// browser's own v86 instance, proving NE2000 unlocked the same real fetch
// path in the browser that RTL8139 already had natively.
//
// Usage: node tools/checks/ne2k-check.mjs [url]
import { chromium } from 'playwright';

const url = process.argv[2] || 'http://localhost:8934/index.html';
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.goto(url, { waitUntil: 'load' });

await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
console.log('v86 adapters ready');

// Poll the kernel's own serial log for the real evidence lines, same
// budget geo-check.sh gives native QEMU (weather cycle fires on the first
// idle-loop frame after boot, then a real ARP+DNS+TCP round trip through
// v86's own emulated network relay to the host).
const deadline = Date.now() + 240000;
let serial = '';
let sawWallErr = false;
while (Date.now() < deadline) {
  serial = await page.evaluate(() => (window.__jt && window.__jt.serial) || '');
  if (/geo=/.test(serial) && /wx=/.test(serial)) break;
  if (/wallerr=/.test(serial)) { sawWallErr = true; break; }
  await page.waitForTimeout(1000);
}

console.log('--- kernel serial log (tail) ---');
console.log(serial.slice(-2000));
console.log('--- end serial log ---');

const geoMatch = serial.match(/geo=([^\r\n]+)/);
const urlMatch = serial.match(/wxurl=([^\r\n]+)/);
const wxMatch = serial.match(/wx=([^\r\n]+)/);

await browser.close();

if (geoMatch && urlMatch && wxMatch) {
  console.log('PASS: real network fetch through v86 NE2000 succeeded');
  console.log('  geo:   ' + geoMatch[1]);
  console.log('  wxurl: ' + urlMatch[1]);
  console.log('  wx:    ' + wxMatch[1]);
  process.exit(0);
} else if (sawWallErr) {
  console.log('FAIL: wall_fetch reported a real error (wallerr= line above), NIC path may be up but the fetch itself failed');
  process.exit(1);
} else {
  console.log('FAIL: no geo=/wxurl=/wx= lines landed in the serial log within budget, network fetch did not complete');
  process.exit(1);
}
