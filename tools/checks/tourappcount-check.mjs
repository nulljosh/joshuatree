// Static, discriminating check for the idle tour's own scope. v0.71.3
// trimmed this to 1-3 apps ("one or two, or maybe three"); v0.72.2 restored
// all 8 real dock apps per a direct follow-up ("looks through every app
// properly, only the first half in the dock"), reconciled with the earlier
// ask by cutting DWELL_MS instead of the app count. Asserts all 8 real
// dock apps (every DOCK_SLOTS entry except the Apps folder and Trash,
// neither of which the tour ever opened) are named somewhere in
// landing/v86/embed.js's own tour definitions. Proven discriminating the
// same way every check in this repo is: this failed (3 apps) before the
// v0.72.2 restore and passes (8 apps) after it.
//
// v0.76.12: 3 of the 8 (Files, Weather, Reminders) moved out of the flat
// TOUR_APPS array and into MW_FILES/MW_WEATHER/MW_REMINDERS, driven by
// multiWindowRound() instead of the plain sequential loop -- a real
// architecture change (they're genuinely multi-window apps now, shown two
// at a time), not a regression back to v0.71.3's "fewer apps" bug. This
// check's real invariant was always "every real dock app actually gets
// demonstrated by the tour", not "must be a literal entry in the
// TOUR_APPS array specifically", so it now reads every `name: '...'`
// dock-app definition in the file (TOUR_APPS's 5 solo apps plus the 3
// MW_* multi-window ones) and checks the union covers all 8.
import { readFileSync } from 'fs';

const EXPECTED = ['Files', 'Mail', 'Calendar', 'Notes', 'Reminders', 'Terminal', 'Chat', 'Weather'];

const src = readFileSync(new URL('../../landing/v86/embed.js', import.meta.url), 'utf8');
const names = [...src.matchAll(/name:\s*'([^']+)'/g)].map(x => x[1]);
console.log('Tour dock apps (TOUR_APPS + multi-window rounds):', names.join(', '), `(${names.length})`);
const missing = EXPECTED.filter(n => !names.includes(n));
const extra = names.filter(n => !EXPECTED.includes(n));
if (missing.length === 0 && extra.length === 0 && names.length === EXPECTED.length) {
  console.log('PASS: tour cycles all 8 real dock apps');
  process.exit(0);
} else {
  console.log('FAIL: tour app list does not match the 8 expected apps.', missing.length ? `missing: ${missing.join(', ')}` : '', extra.length ? `extra: ${extra.join(', ')}` : '');
  process.exit(1);
}
