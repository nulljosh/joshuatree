# Joshua Tree loop handoff (2026-11-16, TIME)

## What the loop is

Claude works the roadmap's goals toward v15, one pull request at a time, until weekly usage reaches 75% or nothing an emulator can prove is left. Each step is a minor version, each fix a patch. A goal only becomes the next major when its gate truly passes, and a gate that needs the board waits for Joshua's board test. Goal 14 is graphics acceleration; goal 15 is the 64-bit PC port. The goals and gates are in `docs/ROADMAP.md`, under Version plan. The target was 20 until today; Joshua narrowed it to 14.

## Where things stand

- Main is on 13.16.0, released and signed, so the Pi's `update` command can pull it. Joshua has not run `update` or `bench` on the Pi yet; the Mac relay is running with bench on, waiting for the Pi.
- Printing and scanning shipped in 13.16.0 (a note prints to a network printer, printers are found on the network, Cmd+P in Notes, a scan command pulls a page into Photos). Joshua has no printer, so it was not proven on paper.
- PR 1138 (13.17.0, bench's hands and eyes) is open, in CI, auto-merge on: the Mac can press keys, type, move, click and drag the pointer on the Pi, and get a PNG of the Pi's screen back. It passes in the emulator; one landing spec flaked and is rerunning. Do not touch PR 1138 or branch v14/bench2.
- Version 14 is still graphics acceleration on the Pi (the V3D chip drawing the desktop). Its next step waits on Joshua trying `gpu` and `gpu clear` on the real board.
- The 64-bit PC port has started: branch v15/x64-m0 (worktree ~/Documents/Code/jt-x64) has one local commit, NOT pushed. A 64-bit kernel boots into long mode in the emulator, maps 4 GB, and prints on serial and a 1024x768 screen. It ships as 13.18.0 after 1138 merges. Do not touch that branch or worktree.
- Parked until after 14: PR 1133 (Paddle polish) and PR 1128 (auto lock).

## Next, in order

1. Let PR 1138 merge on green (the main session merges it). Do not touch it by hand.
2. Sign each release with tools/update-sign.sh so the Pi's `update` can pull it, starting with 13.17.0 and 13.18.0.
3. Test the board through `bench` once the Pi asks for it: keys, mouse clicks and screenshots from the Mac. Joshua's checks for printing, scanning, the Wi-Fi fix and a big download still count.
4. GPU steps toward v14: the V3D chip drawing the desktop, each step a minor with host checks, proven on the board when Joshua tries `gpu` and `gpu clear`.
5. x86-64 slices toward v15: 13.18.0 is slice 1, each later slice a minor with emulator checks.

## Restart prompt

```
/loop joshuatree: ship 13.17.0 (bench keys, mouse, screenshot) and 13.18.0 (x86-64 slice 1), sign each release, test the board through bench once the Pi asks, then GPU steps toward v14 and x86-64 slices toward v15
```
