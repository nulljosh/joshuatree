// Static, discriminating check for the idle tour's own scope: Joshua's
// direct request was "one or two, or maybe three apps", not a marathon
// through every dock icon. Reads landing/v86/embed.js's real TOUR_APPS
// array (not a copy) and asserts it names at most 3 apps. Proven
// discriminating the same way every check in this repo is: this failed
// (8 apps) before the v0.71.3 trim and passes (3 apps) after it.
import { readFileSync } from 'fs';

const src = readFileSync(new URL('../landing/v86/embed.js', import.meta.url), 'utf8');
const m = src.match(/var TOUR_APPS = \[([\s\S]*?)\n  \];/);
if (!m) { console.log('FAIL: could not find TOUR_APPS in embed.js'); process.exit(1); }
const names = [...m[1].matchAll(/name:\s*'([^']+)'/g)].map(x => x[1]);
console.log('TOUR_APPS:', names.join(', '), `(${names.length})`);
if (names.length >= 1 && names.length <= 3) {
  console.log('PASS: tour cycles ' + names.length + ' apps, matches "one or two, or maybe three"');
  process.exit(0);
} else {
  console.log('FAIL: tour cycles ' + names.length + ' apps, expected 1-3');
  process.exit(1);
}
