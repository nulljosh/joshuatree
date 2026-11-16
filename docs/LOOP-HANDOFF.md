# Joshua Tree loop handoff (2026-11-16, midday)

## What the loop is

Claude works the roadmap's goals toward v14, one pull request at a time, until weekly usage reaches 75% or nothing an emulator can prove is left. Each step is a minor version, each fix a patch. A goal only becomes the next major when its gate truly passes, and a gate that needs the board waits for Joshua's board test. Goal 14 is printing and scanning. The goals and gates are in `docs/ROADMAP.md`, under Version plan. The target was 20 until today; Joshua narrowed it to 14.

## Where things stand

- Main is on 13.9.2. PR 1134 (roadmap handoff docs) is merged. PRs 1121, 1127 and 1130 were closed as superseded.
- PRs 1132 and 1135 were closed and folded into PR 1137 (13.9.3 to 13.16.0, branch v14/bench), in CI with auto-merge on. It carries the board fixes, printing and scanning, the Wi-Fi fix for downloads that stopped at 800 bytes, `gpu clear` (the graphics chip's first job) and `bench`. Do not touch PR 1137 or branch v14/bench.
- Joshua has no printer. Printing ships as 13.x, and version 14 is now graphics acceleration: the V3D chip drawing the desktop, proven on the board.
- `bench` lets the Mac run Terminal commands on the Pi through the relay (relay.py --bench-dir, tools/pi-bench.py), so board tests need no second keyboard. It passes in the emulator and has not run on the board yet. Next for it: key presses, mouse clicks, and a screenshot back.
- A test card with 13.14.1 was flashed today. After 1137 merges, every release must be signed with tools/update-sign.sh so the Pi's `update` command can pull it. 13.16.0 is not signed yet.
- Version 15 (the 64-bit PC port) is not happening today.
- Parked until after 14: PR 1133 (Paddle polish) and PR 1128 (auto lock).

## Next, in order

1. Let PR 1137 merge on green. Do not touch it by hand.
2. Sign 13.16.0 with tools/update-sign.sh so the Pi can update to it.
3. Test the board through `bench`: flash or update the card, then drive key presses, mouse clicks and a screenshot from the Mac. Joshua's board checks for printing, scanning, the Wi-Fi fix and a big download still count.
4. GPU steps toward v14: the V3D chip drawing the desktop, each step a minor with host checks, proven on the board.
5. Park PRs 1133 and 1128 until 14 is done.

## Restart prompt

```
/loop joshuatree: ship 13.16.0, sign it, test the board through bench, then GPU steps toward v14
```
