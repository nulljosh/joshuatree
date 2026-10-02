# Assembling the Strata Kit

You have the printed parts, a board, a power supply and a bag of screws. This takes about 45 minutes. A one-page picture version is in [`BUILD.md`](BUILD.md). Nothing here needs a special tool. If you want to see it go together first, the [30 second ad](https://joshuatree.heyitsmejosh.com/ad/) runs live in 3D.

The parts come from `strata_cad.py`. Every STL is in `stl/`, and `stl/manifest.json` lists how many of each to print. The whole machine is one stack: four rods hold a ring sandwich together, a tray sits in the middle, the board sits in the tray.

## Tools

- A small Phillips driver for the four board screws
- Small pliers or a 5.5 mm nut driver
- A hobby knife and a bit of sandpaper for print blobs
- Superglue, one drop at a time (rear plate only)
- A hacksaw or side cutters if your M3 rods are longer than 65 mm

## Bill of materials

Same lines as the table in `docs/HARDWARE.md`. The ID is what each step points at.

| ID | Part | Qty | Where |
|---|---|---|---|
| P1 | Printed rings, quarters (`ring0` to `ring5`), spacer bosses already on them | 24 pieces | `stl/` |
| P2 | Printed cap quarter, bosses already on it | 4 | `stl/` |
| P3 | Printed tray, board standoffs already on the floor | 1 | `stl/` |
| P4 | Printed rear plate | 1 | `stl/` |
| B1 | ASRock J4125B-ITX board | 1 | listing, ~$120 |
| B2 | 8GB DDR4 SO-DIMM | 1 | in the board |
| B3 | 128GB SATA SSD | 1 | in the tray |
| B4 | PSU, picoPSU or mini-ITX compatible | 1 | rear, plugs into the board |
| B5 | 16GB USB stick, OS pre-flashed | 1 | in the kit |
| B6 | M3 threaded rod, 65 mm | 4 | hardware bag |
| B7 | M3 nut | 8 | hardware bag |
| B8 | M3x8 self-tapping screw (board to the printed standoffs) | 4 | hardware bag |
| B9 | Stock I/O shield | 1 | in the board box |
| B10 | SATA cable | 1 | hardware bag |

## Print settings

All parts print flat on the bed, in the orientation the STL is already in. No part needs more than 8 mm of bridge, so there are no supports anywhere. Bed: 180 x 180 mm (Bambu A1 mini). Nozzle 0.4 mm.

| Part | Layer | Walls | Infill | Supports |
|---|---|---|---|---|
| P1 rings, P2 cap (print boss side up, the STLs are already flipped) | 0.2 mm | 3 | 15% gyroid | none |
| P3 tray | 0.2 mm | 3 | 20% gyroid | none |
| P4 rear plate | 0.2 mm | 3 | 100% | none |

PLA is fine to start. PETG is better if the box will sit near a warm room. Print one tone per ring if you want the terracotta to cream fade: B9542C at the base, F0E7D8 at the top.

## Steps

![Step 1](assembly/step1-feet-and-rods.svg)

**1. Ring 0 and rods.** Lay the four ring 0 pieces (P1, the widest) in a square on the table. Push a rod (B6) down through each corner hole until 5 mm pokes out underneath. Tip the piece up, spin a nut (B7) onto the rod end, and lower it so the nut drops into the hex pocket. Level the rod end with the pocket. There are no feet any more, ring 0 is the base.

![Step 2](assembly/step2-tray.svg)

**2. Tray.** Lower the tray (P3) into the middle of ring 0, between the four rods. It stands on the table. The four standoffs are already printed on its floor.

![Step 3](assembly/step3-rings.svg)

**3. Rings.** Slide ring 1 over the rods. Its little 2 mm bosses rest on ring 0 and make the vent gap. Then ring 2, 3, 4, 5. No loose spacers. The rear pieces with the notch between them are for the I/O.

**4. Board.** Seat the board (B1, with B2 in it) on the printed standoffs and screw it down with the four M3x8 screws (B8). Clip the I/O shield (B9) onto the back of the board. Plug in the SATA cable (B10) and the SSD (B3).

![Step 4](assembly/step4-rear-plate.svg)

**5. Rear plate.** Slide the plate (P4) in behind the board, between the tray's side walls, so the shield pokes through the window. It's a 0.2 mm slip fit. One drop of superglue on each side keeps it there. Plug the power supply (B4) into the board.

![Step 5](assembly/step5-cap-and-nuts.svg)

**6. Cap.** Set the four cap pieces (P2) on ring 5, rod holes over the rods. Drop a nut (B7) into each pocket on top and run it down until the stack is snug. Snug, not crushed. If a ring is rocking, one nut is loose.

**7. First boot.** Plug a keyboard into the PS/2 port, a monitor into the board, and the USB stick (B5) into a rear USB port. Power on and tap F11 (or F2, then boot menu) for the board's boot menu. Pick the USB stick, the UEFI entry. You'll see the GRUB menu, then Joshua Tree. If the screen stays black, plug a USB serial adapter into the board's header and read the log. `docs/HARDWARE.md` Phase 0 has the whole drill.

## If something doesn't fit

Cutting down: 35 prints and 58 loose pieces became 30 prints and 30 pieces, and 20 fasteners became 16. Parts have 0.2 mm of clearance on every mating face. If your printer runs tight, sand the edge, don't scale the part. If a ring hole won't clear the tray, you printed too hot. Fix the part in `strata_cad.py`, run it again, reprint. That loop is the whole point.

---
Strata Kit and Joshua Tree are trademarks of Joshua Trommel. Designs licensed CC BY-NC-SA 4.0. Build one for yourself; please do not sell copies.
