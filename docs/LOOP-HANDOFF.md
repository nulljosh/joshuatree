# Joshua Tree loop handoff (2026-09-30)

## What the loop is

Build Joshua Tree to 2.0.0, one small PR at a time. docs/VERSIONS.md is the map: 1.8, then 1.9, then 2.0. Each agent gets one shippable slice, about 10 minutes, headless QEMU only, one VM at a time. Two agents at once if either is Fable or Opus, otherwise three. PRs stay draft until `tools/ci-local.sh` is green, then merge on green without asking. While a big PR waits, hold every other merge (main requires up to date branches), and fold small docs changes into a PR that is already open. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR as the change. Zero open issues, always. At 90% session usage, checkpoint and stop starting new work.

## The 2.0.0 gate

2.0.0 ships only when all of these are true and each has a headless check in ci-suite.sh:

1. 1.8 done: a phone home screen, an app grid, one app full screen at a time, a back button.
2. 1.9 done: touch works, an on-screen keyboard, every app readable at phone size.
3. Every app in the APPS[] table runs as its own ring 3 program. No app code left in kernel.c.
4. A check crashes each app on purpose and proves the desktop is still alive after every one. Done in 1.9.16 (`tools/checks/ring3crash-all-check.py`, 17 of 17).
5. Input goes to the focused window only, not a global key pull.

## Where things stand

Checkpoint 2026-09-30. Weekly usage hit 75%, so the loop paused. Main is 1.9.10, and 15 of 26 apps run in ring 3.

In flight, each on its own branch:

- #317 `curbfind-ring3` (1.9.11): Curbfind to ring 3, plus SYS_HTTP_GET (387). The kernel fixes the host; the app passes only a checked path.
- `calendar-ring3`: Calendar to ring 3, file calls only. Moves the multi-window checks off Calendar.
- `search-ring3`: Search to ring 3, plus SYS_READDIR (388). Files will reuse it.

Each port bumps VERSION on its own. Renumber at merge, in order: Curbfind 1.9.11, then Calendar, then Search. After each merge, rerun `tools/gen/inject-landing-facts.py` so the landing's ring-3 count stays right.

Building on Linux changes every other app's committed `.bin` and `drivers/user_*.h`. Never commit those; `git checkout` them.

## Next, in order

1. Land whatever in-flight branch is pushed and green, one at a time, renumbering as above.
2. Weather, Stocks and Epiphany to ring 3 as one batch, all on SYS_HTTP_GET.
3. Tour scenes for Activity and the Apps folder (landing/v86/embed.js, tourappcount-check.mjs).
4. The big four: Files (on SYS_READDIR), Terminal, Notes, Mail, then Samantha.
5. 1.9: touch and an on-screen keyboard, every app readable at phone size.
6. 2.0: input by focus, then tag 2.0.0.
7. 2.1 Music, 2.2 Video, 3.0 on the ASRock J4125B-ITX, the Strata Kit at 3.1.

## Restart prompt

```
/loop build Joshua Tree to 2.0.0: docs/LOOP-HANDOFF.md has the gate and the order. One ~10 minute slice per agent, two at once if Fable, headless only, draft until ci-local is green, merge on green, hold merges while a big PR waits, zero issues, docs and landing in the same PR, MONEY.md dated line each pitch. Stop starting work at 90% usage.
```
