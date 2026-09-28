# Joshua Tree loop handoff (2026-09-28, after midnight)

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

Main is 1.7.5 and live. 1.7.4 brought the OS its own network address (DHCP), drunk mode, the start of voice input, and landing polish. 1.7.5 fixed the deploy: a rate limit setting in the old format had kept the live site on 1.7.3.

Open: #268 (1.7.6, the kernel goes 1-bit with ordered dither; local suite running before it goes ready) and #267 (this file and MONEY.md). A Fable agent is on 2.0 step one: window and input syscalls for ring 3, Keyrate as the first app moved out, and a crash that returns to the desktop.

kernel.c is 9,877 lines with every app still inside it. That number going down is the real progress bar for 2.0.

## Next, in order

1. Land #268, then the ring 3 Keyrate PR.
2. Move apps out a few per PR, smallest first, each one added to the crash check.
3. 1.8 phone home screen, in parallel with the moves (it only touches the desktop, not the apps).
4. 1.9 touch and the on-screen keyboard.
5. Before any encryption work: a real randomness pool in the kernel. Today's random numbers are predictable and they salt the passwords. After that, BearSSL on TLS 1.2 (decided 2026-09-28), ported, never hand written.

## Restart prompt

```
/loop build Joshua Tree to 2.0.0: docs/LOOP-HANDOFF.md has the gate and the order. One ~10 minute slice per agent, two at once if Fable, headless only, draft until ci-local is green, merge on green, hold merges while a big PR waits, zero issues, docs and landing in the same PR, MONEY.md dated line each pitch. Stop starting work at 90% usage.
```
