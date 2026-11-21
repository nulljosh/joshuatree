# Joshua Tree loop handoff (2026-11-21, night)

## What the loop is

Claude works the roadmap's goals one pull request at a time until weekly usage reaches 75% or nothing an emulator can prove is left. Each step is a minor version, each fix a patch. A goal becomes the next major only when its gate truly passes; a gate that needs the board waits for Joshua's board test. The goals and gates are in `docs/ROADMAP.md`, under Version plan.

## Where things stand

- 13.21.5 (PR 1174, which folded in 1172): the landing hero is one painted card (the park at dusk, the line in its sky, glass tiles, a floating nav pill), the README is a few lines, `architecture.svg` is three panels, and `tools/gen/gen_stickers.py` draws the sticker sheet in `docs/brand/stickers/`. Ten of each of two stickers (the painted tree and the `joshua@tree %` prompt) are ordered from Sticker Mule, due Dec 9.
- The demo tour now names apps in the hero's readout, not the H1. It still needs Joshua's QA pass in real Chrome.
- Releases after 13.16.0 need signing with `tools/update-sign.sh` so the Pi's `update` can pull them; sign the latest after each merge.
- Still open from before: PR 1166 (gpu fill and copy, x86-64 interrupts), 1139 (old handoff, superseded by this file), 1133 (Paddle polish) and 1128 (auto lock), parked; 1117 and 1114, stale drafts.
- Weekly usage was 77% on 2026-11-21; it resets about 2026-11-23.

## Next, in order

1. Joshua's weekend board test: `docs/TESTING.md`, steps 37 (the bench) and 30 to 36.
2. v14, graphics acceleration: land PR 1166, then the V3D drawing the desktop, step by step, each proven on the board.
3. v15, the 64-bit PC: x86-64 slices 2 to 5 (the Raspberry Pi and ARM64 section of `docs/ROADMAP.md`).
4. Close 1139 and the stale drafts; decide on 1133 and 1128 after 14.

## Restart prompt

```
/loop joshuatree: land PR 1166 and sign the release, then GPU steps toward v14 and x86-64 slice 2 toward v15; stop at 75% weekly usage
```
