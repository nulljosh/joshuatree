# Joshua Tree loop handoff (2026-11-15, morning)

## What the loop is

Claude works the roadmap's goals after 13, one pull request at a time, until weekly usage reaches 75% or nothing an emulator can prove is left. Each step is a minor version, each fix a patch. A goal only becomes the next major when its gate truly passes, and a gate that needs the board waits for Joshua's board test. The goals and gates are in `docs/ROADMAP.md`, under Version plan, 11 to 20.

## Where things stand

- 13.10.0, auto lock, is in PR 1128. With 13.9.0's dim and screen saver, the Energy Saver note is done.
- The landing film is the Fable cut (13.9.1). PR 1127 swaps in the fix: real screens only, a bell on every cut.
- Main has an intermittent Settings check failure on the second boot (a permission fault at 0x44080000). It is being chased first.
- Goal 15 was "an ARM port", already true since 3.0; it is now the 64-bit PC.

## Next, in order

1. Fix the Settings check's second-boot fault, then land 1127 and 1128.
2. Printing: a `print` command that sends a note to an IPP printer, checked against a stub printer on the Mac.
3. Printer discovery over mDNS, then Print in Notes, then scanning over eSCL. That set is the printing goal's gate in QEMU.
4. HiDPI at 2x, the emulator half of the displays goal.
5. Each major, without asking: fresh landing screenshots of the real build, a Fable trailer of real screens with a sound on every cut, a Mobbin pass, and the roadmap's Latest line.

## Restart prompt

```
/loop Joshua Tree: work toward v20 through the roadmap's goals 14 to 20 (docs/ROADMAP.md, Version plan), honest semver: a minor per step, a patch per fix, a major only when a goal's gate truly passes (board-only gates wait for Joshua's board test). Emulator-provable goals first (printing over IPP to a stub, HiDPI), board-only goals (GPU, second HDMI, battery) ship steps as minors with host checks and a TESTING.md board step. One PR per item off origin/main, local checks green, open ready, merge on green, no --admin. Each major, unasked: fresh landing screenshots (tools/landing-shots.py), a fable-ad trailer of real screens with an audible hit on every cut; fix the roadmap Latest line. Keep docs slim and human, update the book. Keep docs/LOOP-HANDOFF.md current. Stop at weekly usage 75% or when nothing provable is left.
```
