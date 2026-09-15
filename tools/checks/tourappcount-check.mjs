// Static, discriminating check for the idle tour's own scope. v0.71.3
// trimmed this to 1-3 apps ("one or two, or maybe three"); v0.72.2 restored
// all 8 real dock apps per a direct follow-up ("looks through every app
// properly, only the first half in the dock"), reconciled with the earlier
// ask by cutting DWELL_MS instead of the app count. Reads landing/v86/
// embed.js's real TOUR_APPS array (not a copy) and asserts it names all 8
// real dock apps (every DOCK_SLOTS entry except the Apps folder and Trash,
// neither of which the tour ever opened). Proven discriminating the same
// way every check in this repo is: this failed (3 apps) before the
// v0.72.2 restore and passes (8 apps) after it.
import { readFileSync } from 'fs';

const EXPECTED = ['Files', 'Mail', 'Calendar', 'Notes', 'Reminders', 'Terminal', 'Chat', 'Weather'];

const src = readFileSync(new URL('../../landing/v86/embed.js', import.meta.url), 'utf8');
const m = src.match(/var TOUR_APPS = \[([\s\S]*?)\n  \];/);
if (!m) { console.log('FAIL: could not find TOUR_APPS in embed.js'); process.exit(1); }
const names = [...m[1].matchAll(/name:\s*'([^']+)'/g)].map(x => x[1]);
console.log('TOUR_APPS:', names.join(', '), `(${names.length})`);
const missing = EXPECTED.filter(n => !names.includes(n));
const extra = names.filter(n => !EXPECTED.includes(n));
if (missing.length === 0 && extra.length === 0 && names.length === EXPECTED.length) {
  console.log('PASS: tour cycles all 8 real dock apps');
  process.exit(0);
} else {
  console.log('FAIL: tour app list does not match the 8 expected apps.', missing.length ? `missing: ${missing.join(', ')}` : '', extra.length ? `extra: ${extra.join(', ')}` : '');
  process.exit(1);
}
