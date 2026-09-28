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

Main is 1.7.11 and live. 1.7.12 puts a third app in ring 3: Calculator runs as its own program next to Keyrate and Toroid, still one row in `kernel/ring3app.c`'s table-driven launcher (name, embedded binary, VFS filename), and the in-kernel copy is deleted. Three apps out, the rest still in kernel.c. That number going down is the real progress bar for 2.0. Quotes is next.

## Next, in order

1. Move passwords from salted hashes to PBKDF2.
2. Call paging_clear_user on window release so a closed app's memory doesn't linger.
3. Audit every syscall for unchecked user pointers.
4. Move apps out a few per PR, smallest first, each one added to the crash check.
5. 1.8 phone home screen, in parallel with the moves (it only touches the desktop, not the apps).
6. 1.9 touch and the on-screen keyboard.

This is a personal repo with no merge queue, so PRs get batched into one release branch instead of landing one at a time.

## Restart prompt

```
/loop build Joshua Tree to 2.0.0: docs/LOOP-HANDOFF.md has the gate and the order. One ~10 minute slice per agent, two at once if Fable, headless only, draft until ci-local is green, merge on green, hold merges while a big PR waits, zero issues, docs and landing in the same PR, MONEY.md dated line each pitch. Stop starting work at 90% usage.
```
