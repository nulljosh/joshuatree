# Joshua Tree loop handoff (2026-09-27, late night)

## What the loop is

Build Joshua Tree toward its own computer and a Bloomberg terminal, one PR at a time, watching usage and RAM. Up to 3 Sonnet agents or 2 if one is Opus, 20 minutes each, headless QEMU only, one VM at a time. PRs stay draft until `tools/ci-local.sh` is green. Merge on green, never ask, but while a big PR waits, hold every other merge (main requires up-to-date branches). Keep README, landing, docs/TESTING.md and benchmarks current in the same PR as the change.

## Where things stand

Main is 1.6.20 and live. Merge queue, in order: 239 (Samantha's video face, Chat renamed Samantha; green), then drafts 241 (face loops, face_bench, face_visemes), 243 (Samantha reads mail, notes, reminders), 245 (dev kit waitlist), 246 (boot straight into Samantha, phone mode), 248 (crash reports name the function), 250 (this docs pass), 251 (landing fits phones), 252 (Epiphany command bar: AAPL GP, AAPL DES). Each claims 1.6.21, so every merge after the first rebumps.

Architecture graded C+: kernel.c is ~9,800 lines, all 25 apps run in the kernel. An Opus agent is building the app interface (`feat/app-interface`); apps move to ring 3 next.

Samantha's face: v27 B is best so far by Joshua's eye, built on `tools/gen/face_visemes.py`. Recipe and failures live in the character-creator skill.

## Next, in order

1. Merge 239, then the drafts one at a time, biggest first, rebumping versions.
2. Finish phone mode (430x932, boots to Samantha). Mobile first by 2026-10-04.
3. App interface lands, then apps move out a few per PR.
4. App smoke check for the 14 untested apps (parked in `git stash` on feat/guard-checks; grid launch fails past the first rows). Use the app interface to launch by name instead.
5. Face: phoneme-timed mouth in the kernel, fed by ElevenLabs alignment from Turing.

## Restart prompt

```
/loop keep building the Joshua Tree roadmap: merge the PR queue one at a time (239 first, hold other merges), finish phone mode, the app interface and apps out of the kernel, keep tests, docs, README, landing and benchmarks current, watch RAM and usage. Goal: A+ architecture, mobile first by 2026-10-04.
```
