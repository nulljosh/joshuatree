# Loop handoff (2026-11-16)

## What the loop is

Claude works the roadmap toward v14, one pull request at a time. Each step is a minor version and each fix a patch. A goal becomes a major only when its gate passes. A gate that needs the board waits for Joshua's test. The goals are in `docs/ROADMAP.md`, under Version plan. The target was 20 until today. Joshua narrowed it to 14, printing and scanning.

## Where things stand

- Main is on 13.9.2. PR 1134 (roadmap handoff docs) is merged. PRs 1121, 1127 and 1130 were closed as superseded.
- PR 1137 (13.9.3 to 13.16.0, branch `v14/bench`) carries the board fixes, printing and scanning, the Wi-Fi download fix, `gpu clear` and `bench`. It is in CI with auto-merge on. Do not touch it or the branch by hand.
- Joshua has no printer. Printing ships as 13.x. Version 14 is graphics acceleration: the V3D chip draws the desktop.
- `bench` lets the Mac run Terminal commands on the Pi through the relay (`relay.py --bench-dir`, `tools/pi-bench.py`). It passes in the emulator and has not run on the board yet.
- A 13.14.1 test card was flashed today. Every release must be signed with `tools/update-sign.sh` so the Pi's `update` can pull it. 13.16.0 is not signed yet.
- Version 15 (the 64-bit PC port) is not happening today.
- Parked until after 14: PR 1133 (Paddle polish) and PR 1128 (auto lock).

## Next, in order

1. Let PR 1137 merge on green.
2. Sign 13.16.0 with `tools/update-sign.sh`.
3. Test the board through `bench`: key presses, clicks and a screenshot from the Mac. Joshua's checks for printing, scanning, the Wi-Fi fix and a big download still count.
4. GPU steps toward v14, each a minor with host checks, proven on the board.
5. Park PRs 1133 and 1128 until 14 is done.

## Restart prompt

```
/loop joshuatree: ship 13.16.0, sign it, test the board through bench, then GPU steps toward v14
```
