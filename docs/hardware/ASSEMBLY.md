# Assembling the Neo Kit

You have the printed parts, a board, a power supply and a bag of screws. This takes about 45 minutes. A one-page picture version is in [`BUILD.md`](BUILD.md). Nothing here needs a special tool. If you want to see it go together first, the [30 second ad](https://joshuatree.heyitsmejosh.com/ad/) runs live in 3D.

The parts come from `neo_cad.py`. Every STL is in `stl/`, and `stl/manifest.json` lists how many of each to print. The whole machine is one stack: four rods hold a ring sandwich together, a tray sits in the middle, the board sits in the tray.

## Tools

- A small Phillips driver for the four board screws
- Small pliers or a 5.5 mm nut driver
- A hobby knife and a bit of sandpaper for print blobs
- Superglue, one drop at a time (rear plate only)
- A hacksaw or side cutters if your M3 rods are longer than 50 mm

## Bill of materials

Same lines as the table in `docs/HARDWARE.md`. The ID is what each step points at.

| ID | Part | Qty | Where |
|---|---|---|---|
| P1 | Printed ring pieces (`ring0_*` to `ring5_*`, three per ring: `_front` is the whole 200 mm front with side returns, `_rl` and `_rr` are the rear L pieces), spacer pads already on rings 1 to 5 | 18 pieces | `stl/` |
| P2 | Printed cap: `cap_panel` (the whole tree, engraved) and three `cap_frame_*` pieces (front U, two rear L) | 4 | `stl/` |
| P3 | Printed tray, board standoffs already on the floor | 1 | `stl/` |
| P4 | Printed rear plate, with its outer face flush to the rings | 1 | `stl/` |
| P5 | Foot, TPU or PLA, recessed under ring 0 | 4 | `stl/` |
| B1 | ASRock J4125B-ITX board | 1 | listing, ~$120 |
| B2 | 8GB DDR4 SO-DIMM | 1 | in the board |
| B3 | 128GB SATA SSD | 1 | in the tray |
| B4 | PSU, picoPSU or mini-ITX compatible | 1 | rear, plugs into the board |
| B5 | 16GB USB stick, OS pre-flashed | 1 | in the kit |
| B6 | M3 threaded rod, 50 mm | 4 | hardware bag |
| B7 | M3 nut | 8 | hardware bag |
| B8 | M3x8 self-tapping screw (board to the printed standoffs) | 4 | hardware bag |
| B9 | Stock I/O shield | 1 | in the board box |
| B10 | SATA cable | 1 | hardware bag |

## Print settings

All parts print flat on the bed, in the orientation the STL is already in. No part needs more than 10 mm of bridge, so there are no supports anywhere. Bed: 180 x 180 mm (Bambu A1 mini). Nozzle 0.4 mm.

| Part | Layer | Walls | Infill | Supports |
|---|---|---|---|---|
| P1 rings, P2 cap (print pad side up, the STLs are already flipped; each `_front` file is already turned 45 degrees so the 200 mm front lies diagonally on the 180 mm bed) | 0.2 mm | 3 | 15% gyroid | none |
| P3 tray | 0.2 mm | 3 | 20% gyroid | none |
| P4 rear plate | 0.2 mm | 3 | 100% | none |

PLA is fine to start. PETG is better if the box will sit near a warm room. Print each ring in its own colour. The hex per part is in `stl/manifest.json` and in the table in `BUILD.md`: ring 0 B9542C, ring 1 C26C49, ring 2 CB8565, ring 3 D49E82, ring 4 DEB69F, ring 5 E7CEBB, cap F0E7D8, tray, rear plate and feet 1E1C1A.

## Steps

![Step 1](assembly/step1-feet-and-rods.svg)

**1. Ring 0 and rods.** Lay the three ring 0 pieces (P1, the front and two rear L pieces, 200 mm like every ring) in a square on the table. Drop a nut (B7) into the hex pocket at each corner from below, then push a rod (B6) up through the hole into it. Press a foot (P5) into the round recess under each corner so it closes the pocket and covers the rod end. Nothing metal shows from underneath.

![Step 2](assembly/step2-tray.svg)

**2. Tray.** Lower the tray (P3) into the middle of ring 0, between the four rods. The four standoffs are already printed on its floor. Its walls run up flush with the top of ring 5.

![Step 3](assembly/step3-rings.svg)

**3. Rings.** Slide ring 1 over the rods. Every piece carries its own 2 mm pads (five under the front, three under each rear L: one at the rod corner along the inner chamfer, one near each arm end, one mid-side), so no piece can drop onto the ring below. Then ring 2, 3, 4, 5. No loose spacers. The front of every ring is one unbroken piece; the cuts land on the side faces at 50 and 40 mm from the front, alternating, so the seams stagger. The pads hug the core, 8.4 mm behind the face, so the 2 mm gap shows only shadow and the corners and the rear stay open for air. The rear pieces with the notch between them are for the I/O. Ring 0 is notched 1.6 mm deep as well, so the plate sits in a recess and shows an even black frame on all four sides.

**4. Board.** Seat the board (B1, with B2 in it) on the printed standoffs and screw it down with the four M3x8 screws (B8). Clip the I/O shield (B9) onto the back of the board. Plug in the SATA cable (B10) and the SSD (B3).

![Step 4](assembly/step4-rear-plate.svg)

**5. Rear plate.** Lower the plate (P4) from above into the notch at the back of the rings, behind the board, so the shield sits in the window. Its outer face ends flush with the rings, and its top sits level with the cap panel underside, rising to the cap frame rebate behind it. It's a 0.2 mm slip fit. One drop of superglue on each side keeps it there. Plug the power supply (B4) into the board.

![Step 5](assembly/step5-cap-and-nuts.svg)

**6. Top nuts and cap.** Before the cap, drop a nut (B7) into each of the four hex pockets in the top of ring 5 and run it down until the stack is snug. Snug, not crushed. If a ring is rocking, one nut is loose. Then set the three frame pieces (P2) flat on ring 5 with one drop of superglue under each. Lay the cap panel (the tree) in the middle, resting on the tray walls. The frame hides every nut, and no seam crosses the tree.

**7. First boot.** Plug a keyboard into the PS/2 port, a monitor into the board, and the USB stick (B5) into a rear USB port. Power on and tap F11 (or F2, then boot menu) for the board's boot menu. Pick the USB stick, the UEFI entry. You'll see the GRUB menu, then Joshua Tree. If the screen stays black, plug a USB serial adapter into the board's header and read the log. `docs/HARDWARE.md` Phase 0 has the whole drill.

## If something doesn't fit

Cutting down: 35 prints and 58 loose pieces became 28 printed pieces in 25 files, and 20 fasteners became 16. Parts have 0.2 mm of clearance on every mating face. If your printer runs tight, sand the edge, don't scale the part. If a ring hole won't clear the tray, you printed too hot. Fix the part in `neo_cad.py`, run it again, reprint. That loop is the whole point.

---
Neo Kit and Joshua Tree are trademarks of Joshua Trommel. Designs licensed CC BY-NC-SA 4.0. Build one for yourself; please do not sell copies.
