#!/usr/bin/env node
// v0.76.10: direct report, "landing page demo says wrong time, should say
// today but says tomorrow." Real, root-caused: libv86.js's own CMOS/RTC
// device (class hb) always answers reads in UTC (getUTCHours/getUTCDate/
// etc, hardcoded, confirmed by reading its source directly), unlike real
// hardware or QEMU's CLI, which usually hand the guest local time
// inherited from the host. Fixed in embed.js's localRtcTime() by shifting
// the RTC's live epoch-ms counter by the browser's own local UTC offset,
// once at boot, so its UTC-labeled reads end up reporting real local time.
//
// This is a pure-math unit test of that formula, not a full end-to-end
// browser+v86 boot (this CI container has no Playwright/Chromium wired up
// -- confirmed manually instead with a real headless Chromium run using
// Playwright's timezoneId context option simulating "America/Los_Angeles":
// booted the exact kernel.elf, screenshotted the menu bar, and it read
// "Tue Sep 15  5:28 PM" against a real host local time of "Tue Sep 15 2026
// 17:28:04 GMT-0700 (Pacific Daylight Time)" -- exact match. Reverting the
// fix and rerunning the identical screenshot showed "Wed Sep 16  12:28 AM"
// (raw UTC), the precise "tomorrow" bug reported, then restored clean).
import { localRtcTime } from "../../landing/v86/embed.js";

let failures = 0;
function check(label, actual, expected) {
  if (actual !== expected) {
    console.log(`FAIL: ${label}: expected ${expected}, got ${actual}`);
    failures++;
  } else {
    console.log(`ok: ${label}`);
  }
}

// US Pacific in September (PDT, UTC-7): getTimezoneOffset() returns +420
// (positive = behind UTC). A UTC instant of 2026-09-16T00:28:00Z is really
// 2026-09-15 17:28 local -- the exact "already tomorrow in UTC, still
// today locally" shape from the real report above.
{
  const utcNow = Date.UTC(2026, 8, 16, 0, 28, 0); // 2026-09-16 00:28:00 UTC
  const shifted = localRtcTime(utcNow, 420);
  const d = new Date(shifted);
  check("PDT (UTC-7) date rolls back to the 15th", d.getUTCDate(), 15);
  check("PDT (UTC-7) hour reads 17 (5 PM)", d.getUTCHours(), 17);
  check("PDT (UTC-7) minute unchanged", d.getUTCMinutes(), 28);
}

// A zone ahead of UTC (JST, UTC+9): getTimezoneOffset() returns -540
// (negative = ahead of UTC). Real local time is already the 16th while
// UTC is still on the 15th late at night -- the mirror-image case.
{
  const utcNow = Date.UTC(2026, 8, 15, 23, 10, 0); // 2026-09-15 23:10:00 UTC
  const shifted = localRtcTime(utcNow, -540);
  const d = new Date(shifted);
  check("JST (UTC+9) date rolls forward to the 16th", d.getUTCDate(), 16);
  check("JST (UTC+9) hour reads 8 AM", d.getUTCHours(), 8);
  check("JST (UTC+9) minute unchanged", d.getUTCMinutes(), 10);
}

// A fractional-hour zone (IST, UTC+5:30): getTimezoneOffset() returns -330.
{
  const utcNow = Date.UTC(2026, 8, 15, 12, 0, 0);
  const shifted = localRtcTime(utcNow, -330);
  const d = new Date(shifted);
  check("IST (UTC+5:30) hour reads 17 (5:30 PM)", d.getUTCHours(), 17);
  check("IST (UTC+5:30) minute reads 30", d.getUTCMinutes(), 30);
}

// UTC itself (offset 0): must be a true no-op, not an accidental shift.
{
  const utcNow = Date.UTC(2026, 8, 15, 12, 0, 0);
  check("UTC (offset 0) is a no-op", localRtcTime(utcNow, 0), utcNow);
}

if (failures > 0) {
  console.log(`FAIL: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("PASS: localRtcTime shifts a UTC-based RTC reading to the real local wall clock across UTC-7, UTC+9, UTC+5:30, and UTC+0");
