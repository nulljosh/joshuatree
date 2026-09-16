// v0.76.22: static check that verifies the idle-tour autoplay bug is fixed.
// The bug: tourArmed is set to true when the tour starts, but if tourLoop()
// exits early for any reason, tourArmed is never reset. This means the
// setInterval can never arm the tour again, causing autoplay to stop working.
//
// The fix: tourLoop() should have a .finally() handler that resets tourArmed
// when tourLoop completes/exits, so the tour can restart if needed.
//
// This check verifies that the fix is in place by examining landing/v86/embed.js
// for the finally() handler pattern.
import { readFileSync } from 'fs';

const src = readFileSync(new URL('../../landing/v86/embed.js', import.meta.url), 'utf8');

// The fix pattern: tourLoop(tourGen).finally(...).catch(...)
// We need to verify that there's a finally() call that resets tourArmed
const hasFinally = src.includes('.finally(function () { tourArmed = false; })');
const hasCatch = src.includes('.catch(function () {');

if (hasFinally && hasCatch) {
  console.log('PASS: idle tour autoplay fix is in place (.finally resets tourArmed)');
  process.exit(0);
} else {
  console.log('FAIL: idle tour autoplay fix is missing');
  if (!hasFinally) console.log('  missing: .finally handler that resets tourArmed');
  if (!hasCatch) console.log('  missing: .catch error handler');
  process.exit(1);
}
