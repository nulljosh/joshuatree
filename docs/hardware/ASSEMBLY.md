# Assembling the Strata Kit

You have the printed parts, a board, a power supply and a bag of screws. This takes about an hour. Nothing here needs a special tool.

The parts come from `strata_cad.py`. Every STL is in `stl/`, and `stl/manifest.json` lists how many of each to print. The whole machine is one stack: four rods hold a ring sandwich together, a tray sits in the middle, the board sits in the tray.

## Tools

- A 2 mm hex key or small Phillips driver for the board screws
- Small pliers or a 5.5 mm nut driver
- A hobby knife and a bit of sandpaper for print blobs
- Superglue, one drop at a time (rear plate only)
- A hacksaw or side cutters if your M3 rods are longer than 70 mm

## Bill of materials

Same lines as the table in `docs/HARDWARE.md`. The ID is what each step points at.

| ID | Part | Qty | Where |
|---|---|---|---|
| P1 | Printed rings, quarters (`ring0` to `ring5`) | 24 pieces | `stl/` |
| P2 | Printed cap quarter | 4 | `stl/` |
| P3 | Printed tray | 1 | `stl/` |
| P4 | Printed rear plate | 1 | `stl/` |
| P5 | Printed foot | 4 | `stl/` |
| P6 | Printed spacers (one plate of 24) | 24 | `stl/` |
| B1 | ASRock J4125B-ITX board | 1 | listing, ~$120 |
| B2 | 8GB DDR4 SO-DIMM | 1 | in the board |
| B3 | 128GB SATA SSD | 1 | in the tray |
| B4 | PSU, picoPSU or mini-ITX compatible | 1 | rear, plugs into the board |
| B5 | 16GB USB stick, OS pre-flashed | 1 | in the kit |
| B6 | M3 threaded rod, 70 mm | 4 | hardware bag |
| B7 | M3 nut | 8 | hardware bag |
| B8 | M3 standoff, 6 mm, plus 4 M3x6 screws | 4 | hardware bag |
| B9 | Stock I/O shield | 1 | in the board box |
| B10 | SATA cable | 1 | hardware bag |

## Print settings

All parts print flat on the bed, in the orientation the STL is already in. No part needs more than 8 mm of bridge, so there are no supports anywhere. Bed: 180 x 180 mm (Bambu A1 mini). Nozzle 0.4 mm.

| Part | Layer | Walls | Infill | Supports |
|---|---|---|---|---|
| P1 rings, P2 cap | 0.2 mm | 3 | 15% gyroid | none |
| P3 tray | 0.2 mm | 3 | 20% gyroid | none |
| P4 rear plate | 0.2 mm | 3 | 100% | none |
| P5 foot | 0.2 mm | 4 | 100% | none |
| P6 spacers | 0.12 mm | 3 | 100% | none |

PLA is fine to start. PETG is better if the box will sit near a warm room. Print one tone per ring if you want the terracotta to cream fade: B9542C at the base, F0E7D8 at the top.

## Steps

![Step 1](assembly/step1-feet-and-rods.svg)

**1. Feet and rods.** Drop an M3 nut (B7) into the pocket under each foot (P5). Stand the four feet in a square. Thread a rod (B6) up through each one until it bottoms in the nut. The rods stand 70 mm tall.

![Step 2](assembly/step2-tray.svg)

**2. Tray.** Lower the tray (P3) between the four rods. It sits on the table, not on the feet. Screw the four standoffs (B8) into the tray floor from underneath with the M3x6 screws. The standoff holes match the mini-ITX pattern, 154.94 x 157.48 mm.

![Step 3](assembly/step3-rings.svg)

**3. Rings.** Start with ring 0, the widest. It is four pieces: lay them round the tray, rod holes over the rods. Then a spacer (P6) on each rod. Then ring 1. Spacer. Ring 2, and so on to ring 5. The 2 mm gap under every ring is the vent, so don't skip a spacer. The last spacer goes on top of ring 5. The four rear pieces of rings 0 to 4 are the left and right pair with the notch between them, that notch is for the I/O.

**4. Board.** Seat the board (B1, with B2 in it) on the standoffs and screw it down. Clip the I/O shield (B9) onto the back of the board. Plug in the SATA cable (B10) and the SSD (B3).

![Step 4](assembly/step4-rear-plate.svg)

**5. Rear plate.** Slide the plate (P4) in behind the board, between the tray's side walls, so the shield pokes through the window. It's a 0.2 mm slip fit. One drop of superglue on each side keeps it there. Plug the power supply (B4) into the board.

![Step 5](assembly/step5-cap-and-nuts.svg)

**6. Cap.** Set the four cap pieces (P2) on the last spacers, rod holes over the rods. Drop a nut (B7) into each pocket on top and run it down until the stack is snug. Snug, not crushed. If a ring is rocking, one nut is loose.

**7. First boot.** Plug a keyboard into the PS/2 port, a monitor into the board, and the USB stick (B5) into a rear USB port. Power on and tap F11 (or F2, then boot menu) for the board's boot menu. Pick the USB stick, the UEFI entry. You'll see the GRUB menu, then Joshua Tree. If the screen stays black, plug a USB serial adapter into the board's header and read the log. `docs/HARDWARE.md` Phase 0 has the whole drill.

## If something doesn't fit

Parts have 0.2 mm of clearance on every mating face. If your printer runs tight, sand the edge, don't scale the part. If a ring hole won't clear the tray, you printed too hot. Fix the part in `strata_cad.py`, run it again, reprint. That loop is the whole point.
