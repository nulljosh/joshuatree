# Joshua Tree loop handoff (2026-09-30)

## What the loop is

Build Joshua Tree to 2.0.0, one small PR at a time. docs/VERSIONS.md is the map: 1.8, then 1.9, then 2.0. Each agent gets one shippable slice, about 10 minutes, headless QEMU only, one VM at a time. Two agents at once if either is Fable or Opus, otherwise three. PRs stay draft until `tools/ci-local.sh` is green, then merge on green without asking. While a big PR waits, hold every other merge (main requires up to date branches), and fold small docs changes into a PR that is already open. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR as the change. Zero open issues, always. At 90% session usage, checkpoint and stop starting new work.

## The 2.0.0 gate

2.0.0 ships only when all of these are true and each has a headless check in ci-suite.sh:

1. 1.8 done: a phone home screen, an app grid, one app full screen at a time, a back button.
2. 1.9 done: touch works, an on-screen keyboard, every app readable at phone size.
3. Every app in the APPS[] table runs as its own ring 3 program. No app code left in kernel.c.
4. A check crashes each app on purpose and proves the desktop is still alive after every one. Done for every ring-3 app in 1.9.14 (`tools/checks/ring3crash-all-check.py`, 18 of 18).
5. Input goes to the focused window only, not a global key pull.

## Where things stand

Checkpoint 2026-09-30. Main is 1.9.14 once the landing PR merges, and 18 of 26 apps run in ring 3.

- In ring 3: Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar, Reminders, Curbfind, Calendar, Search.
- Still in the kernel: Burrow (was Files), Mail, Notes, Terminal, Samantha, Weather, Stocks, Epiphany.
- Gate item 4 (crash every app) is done for all ring-3 apps. The check parses `RING3_APPS`, so a new port is covered on its own.
- New syscalls: 386 tasks, 387 http_get (Curbfind), 388 readdir (Search, for Files next).
- The 1.9.14 PR also carries the landing work: chat bar on phones, icon buttons, QA fixes, Tech specs, the footer directory.

Lesson: main requires up-to-date branches, so every merge forces the next PR to re-run CI (about 15 minutes). Batch where possible.

Building on Linux changes every other app's committed `.bin` and `drivers/user_*.h`. Never commit those; `git checkout` them.

QA backlog:

- Samantha's back chevron does nothing in the phone boot-into-Samantha view (`chat_boot_samantha_open` does not run `phone_back_zone_tick`).
- Real Esc is swallowed on the desktop demo (`embed.js`), so every "esc closes" hint is unusable there. Hide the F2 and Esc hints on phones.
- Samantha's window closed itself 8 to 14 s after an error reply on desktop (unconfirmed).
- Stale cursor glyph in Search and Stocks for a moment after opening.
- Clock is hard to reach in the desktop Apps folder (scroll is flaky).
- Mail "New message" should be an inline sheet on phones, not a new window.
- Clock icon should be a live analog face.
- Text still looks soft on phones (canvas scale 1.25x on DPR 3, kernel glyph AA).
- README shields badge showed "invalid" (GitHub side is fine). If it persists, use a self-hosted endpoint badge.

## Next, in order

1. Weather, Stocks and Epiphany to ring 3 in one PR on SYS_HTTP_GET. Check `weather_text` in `phone_home.h` and Samantha's weather tool still work.
2. Burrow (the old Files app, renamed in 1.9.14), then Terminal, Notes, Mail, Samantha to ring 3.
3. 1.9 phone work: touch everywhere, an on-screen keyboard (only Notes has one), every app readable at phone size.
4. Input by focus, then tag 2.0.0.
5. Tour scenes for Activity and the Apps folder (`landing/v86/embed.js`, `tourappcount-check.mjs`).
6. 2.1 Music, 2.2 Video, 3.0 on the ASRock J4125B-ITX, the Strata Kit at 3.1.

## Restart prompt

```
/loop build Joshua Tree to 2.0.0: docs/LOOP-HANDOFF.md has the gate and the order. One ~10 minute slice per agent, two at once if Fable, headless only, draft until ci-local is green, merge on green, hold merges while a big PR waits, zero issues, docs and landing in the same PR, MONEY.md dated line each pitch. Stop starting work at 90% usage.
```
