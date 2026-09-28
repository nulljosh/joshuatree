# Joshua Tree loop handoff (2026-09-28, afternoon)

## What the loop is

Build Joshua Tree to 2.0.0, one small PR at a time. docs/VERSIONS.md is the map: 1.8, then 1.9, then 2.0. Each agent gets one shippable slice, about 10 minutes, headless QEMU only, one VM at a time. Two agents at once if either is Fable or Opus, otherwise three. PRs stay draft until `tools/ci-local.sh` is green, then merge on green without asking. While a big PR waits, hold every other merge (main requires up to date branches), and fold small docs changes into a PR that is already open. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR as the change. Zero open issues, always. At 90% session usage, checkpoint and stop starting new work.

## The 2.0.0 gate

2.0.0 ships only when all of these are true and each has a headless check in ci-suite.sh:

1. 1.8 done: a phone home screen, an app grid, one app full screen at a time, a back button.
2. 1.9 done: touch works, an on-screen keyboard, every app readable at phone size.
3. Every app in the APPS[] table runs as its own ring 3 program. No app code left in kernel.c.
4. A check crashes each app on purpose and proves the desktop is still alive after every one.
5. Input goes to the focused window only, not a global key pull.

## Where things stand

Main is 1.8.10. Four apps run in ring 3 (Keyrate, Toroid, Calculator, Quotes); the rest are still in kernel.c, which is 9433 lines under a ceiling that only ratchets down. Phones get a home screen, tap to hear Samantha, and she finishes talking before the tour moves on. Every check picks its own free QMP port once #289 lands.

Merge train landed: #290 done. Waiting to land: #291 docs to 100%, #289 free QMP ports. #296 is the 3.0 hardware blueprint (draft): the Strata enclosure with CAD, drawing and build steps, the two-box money fix (Strata Kit $199, Strata Complete $349), the ad in docs/DEMO.md uploaded and A-graded. Re-record on ElevenLabs blocked (key invalid 401). Landing fix PR queued.

## Next, in order

1. Land the train: #290, #291, #289. Rebump each with the version-stamp helper; resolve real conflicts by hunk, never by taking a whole side.
2. Landing fixes from the grade: put the ad and a Strata render in "Want one?", one app count everywhere, the phone benchmark labels that collide, the unstyled Samantha link, sections visible without scrolling.
3. Ship Bookrank and Homeqi in ring 3 (both have WIP branches), then resume Notes folders.
4. 1.9: touch and an on-screen keyboard, every app readable at phone size.
5. 2.0: the remaining apps to ring 3, smallest first, each with its crash check; input by focus.
6. 2.1 Music, 2.2 Video. Then 3.0 on the ASRock J4125B-ITX, and the Strata Kit at 3.1.

## Restart prompt

```
/loop build Joshua Tree to 2.0.0: docs/LOOP-HANDOFF.md has the gate and the order. One ~10 minute slice per agent, two at once if Fable, headless only, draft until ci-local is green, merge on green, hold merges while a big PR waits, zero issues, docs and landing in the same PR, MONEY.md dated line each pitch. Stop starting work at 90% usage.
```
