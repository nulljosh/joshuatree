# Build a Neo box

![Build sheet](build-sheet.svg)

Five steps. About 45 minutes once you have the parts. About $250 with the board, RAM and drive, and every price here is an estimate until a real quote lands. The long version with every part ID is [Detailed assembly](#detailed-assembly) below.

## The five steps

1. **Base.** Lay the three ring 0 pieces (the front and two rear L pieces) in a square, drop a nut into each corner pocket from below, push a rod up through it, then press a foot into the round recess under each corner. The feet hide the nut and the rod end.
2. **Tray.** Drop the tray in the middle. The board posts are already on its floor. Its walls run flush with the top of ring 5.
3. **Rings.** Slide the rest of the rings down the rods, one on top of the other. Each ring is three pieces: one 200 mm front with a short return on each side, and two rear L pieces. The little 2 mm pads under every piece (five under the front, three under each L) leave the air gap for you and hold it flat at the rods. The front is never cut, and the side seams alternate between rings so none lines up.
4. **Board.** Screw the board onto the four posts, snap its metal back plate on, lower the rear plate into the notch at the back (it ends flush with the rings), plug in power.
5. **Cap.** Drop a nut into each pocket on top of ring 5 and run it down snug. Glue the three frame pieces on ring 5, then lay the centre panel with the tree in the middle on the tray walls. The tree is one piece and no seam crosses it. Plug in the USB stick and boot.

## What you need

| Item | Qty | Where to buy | Estimate |
|---|---|---|---|
| Printed parts (28 pieces, 25 files in `stl/`, 4 of the pieces are feet) | 1 set | JLC3DP or your own printer | $40 to 75 printed, about $9 at home |
| ASRock J4125B-ITX board | 1 | Walmart or Beach Audio listing | about $120 |
| 8GB DDR4 laptop RAM | 1 | any computer shop | about $25 |
| 128GB SATA drive and a SATA cable | 1 | any computer shop | about $20 |
| Small power supply for mini-ITX (picoPSU) | 1 | any computer shop | about $30 |
| M3 threaded rod 50 mm (4), M3 nuts (8), M3x8 screws (4) | 1 bag | hardware store or Amazon | about $7 |
| 16GB USB stick with Joshua Tree on it | 1 | ships in the kit | about $8 |

Total about $250, mostly estimate. Only the board price comes from a real listing.

## Colours, one filament per row

The fade is the product, so print each ring in its own colour. All seven come from one maker's line, **Bambu Lab PLA Matte**, the matte PLA made for the A1 mini. The hex is the one on Bambu's published filament listing, so check the swatch when you buy. `stl/manifest.json` carries the same hex for every file, and the renders use them.

| Parts | Bambu Lab PLA Matte | Hex |
|---|---|---|
| ring 0 (3 pieces) | Terracotta | `#B15533` |
| ring 1 (3) | Caramel | `#AE835B` |
| ring 2 (3) | Latte Brown | `#D3B7A7` |
| ring 3 (3) | Bone White | `#CBC6B8` |
| ring 4 (3) | Desert Tan | `#E8DBB7` |
| ring 5 (3) | Ivory White | `#FFFFFF` |
| cap panel and cap frame (4) | Desert Tan | `#E8DBB7` |
| tray, rear plate, 4 feet | any black PLA | `#1E1C1A` |

## Pick your path

### I want it done for me

1. Upload every file in `stl/` to jlc3dp.com (3D Printing). Set each quantity from `stl/manifest.json`.
2. JLC3DP does not match a hex colour, so the fade is a home-print feature. An order there gets you the right shape in the one colour you pick for the whole set, and you can paint or swap rings later.
3. Order the board from the listing above, with the rest of the table.
4. When the box arrives, follow the five steps. You need a small Phillips screwdriver and nothing else. Nuts go in by hand.

### I want to print it myself

1. Printer: Bambu A1 mini, 0.4 mm nozzle, PLA or PETG, no supports.
2. Every file prints flat, exactly as it is. Rings and cap: 0.2 mm layers, 3 walls, 15% infill. Tray: 20% infill. Rear plate: 100% infill. Feet: TPU if you have it, PLA works.
3. Print each file in the colour from the table, the number of times `manifest.json` says. About 450 g of plastic and around 30 hours, both estimates.
4. Sand any blob off the edges. Do not scale a part.

## Something went wrong

| It does this | Do this |
|---|---|
| A ring will not slide down | Sand the rod hole edge. Do not scale the file. |
| The stack rocks | One nut is loose. Lift the cap, tighten the four top nuts a little at a time, glue it back. |
| The back plate will not go in | Sand its edges. It is a 0.2 mm fit. |
| The board screws will not bite | Start each screw with a firm push and two turns. The posts are plastic. |
| Black screen on first boot | Check the USB stick is in a rear port, tap F11, pick the stick with UEFI in its name. |


## Detailed assembly

The parts come from `neo_cad.py`. Every STL is in `stl/`, and `stl/manifest.json` lists how many of each to print. The whole machine is one stack: four rods hold a ring sandwich together, a tray sits in the middle, the board sits in the tray.

### Tools

- A small Phillips driver for the four board screws
- Small pliers or a 5.5 mm nut driver
- A hobby knife and a bit of sandpaper for print blobs
- Superglue, one drop at a time (rear plate only)
- A hacksaw or side cutters if your M3 rods are longer than 50 mm

### Bill of materials

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

### Print settings

All parts print flat on the bed, in the orientation the STL is already in. No part needs more than 10 mm of bridge, so there are no supports anywhere. Bed: 180 x 180 mm (Bambu A1 mini). Nozzle 0.4 mm.

| Part | Layer | Walls | Infill | Supports |
|---|---|---|---|---|
| P1 rings, P2 cap (print pad side up, the STLs are already flipped; each `_front` file is already turned 45 degrees so the 200 mm front lies diagonally on the 180 mm bed) | 0.2 mm | 3 | 15% gyroid | none |
| P3 tray | 0.2 mm | 3 | 20% gyroid | none |
| P4 rear plate | 0.2 mm | 3 | 100% | none |

PLA is fine to start. PETG is better if the box will sit near a warm room. Print each ring in its own colour. The hex per part is in the colour table above and in `stl/manifest.json`.

### Steps

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

### If something doesn't fit

Cutting down: 35 prints and 58 loose pieces became 28 printed pieces in 25 files, and 20 fasteners became 16. Parts have 0.2 mm of clearance on every mating face. If your printer runs tight, sand the edge, don't scale the part. If a ring hole won't clear the tray, you printed too hot. Fix the part in `neo_cad.py`, run it again, reprint. That loop is the whole point.

---
Neo Kit and Joshua Tree are trademarks of Joshua Trommel. Designs licensed CC BY-NC-SA 4.0. Build one for yourself; please do not sell copies.
