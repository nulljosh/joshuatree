# Build a Strata box

![Build sheet](build-sheet.svg)

Five steps. About 45 minutes once you have the parts. About $250 with the board, RAM and drive, and every price here is an estimate until a real quote lands. The long version with every part ID is [`ASSEMBLY.md`](ASSEMBLY.md).

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
