# Hardware

3.0 is "it boots a real computer." One reference mini PC: UEFI, USB
keyboard, mouse and stick, a real disk, a real network card, real sound.
This page picks that board, lists the driver work between here and
there, and says what a dev kit costs.

Everything the kernel talks to today is a QEMU-emulated device: rtl8139
or ne2k NIC, Sound Blaster 16, ATA PIO disk, PS/2 keyboard and mouse, a
multiboot/VBE linear framebuffer. None of those chips ship on a board
you can buy in 2026. This page is the bridge.

## The pick

**ASRock J4125B-ITX** (Gemini Lake Refresh, Intel Celeron J4125, mini-ITX,
current on ASRock's site: <https://www.asrock.com/mb/Intel/J4125B-ITX/index.asp>).

Why this one over a fixed-board SBC like an ODROID: it is a real
motherboard you buy new, drop into a case with a PSU and a SATA drive,
and it still ships with **separate PS/2 keyboard and mouse ports on the
rear I/O** (source: search results for the J4125-ITX manual, same rear
I/O layout carried into the B revision -
<https://www.manualslib.com/manual/3159314/Asrock-J4125-Itx.html> -
unverified against the B-revision manual directly, flagged below). PS/2
means the kernel's existing PS/2 driver can bring up input on day one,
before xHCI/HID exists at all. Hobbyist SBCs like the ODROID-H2/H3/H4
series are USB-only for input and, per Hardkernel's own forum, do not
support CSM/legacy boot (source:
<https://forum.odroid.com/viewtopic.php?t=39298>) - worse for this
kernel, not better, despite being the more obvious "small dev board"
answer.

| Chip | Model | Source | Confidence |
|---|---|---|---|
| CPU/SoC | Intel Celeron J4125 (Gemini Lake Refresh) | ASRock spec page | sourced |
| NIC | Realtek RTL8111H (Gigabit, PCIe) | ASRock spec page (J4125-ITX, carried to B) | sourced |
| Audio codec | Realtek ALC892, 7.1-channel HD Audio | ASRock spec page | sourced |
| SATA | 2x native SATA3 (SoC) + 2x SATA3 via ASMedia ASM1061 | ASRock spec page | sourced |
| USB | USB 3.2 Gen1 rear ports + USB 2.0 headers | ASRock spec page | sourced |
| Input (rear I/O) | Separate PS/2 keyboard port + PS/2 mouse port | J4125-ITX manual (ManualsLib mirror) | unverified for the B-revision board specifically |
| Legacy BIOS/CSM | AMI UEFI BIOS, CSM availability not confirmed for Gemini Lake Refresh boards | ASRock forum reports CSM missing on the related J4105/J5005 boards (<https://forum.asrock.com/forum_posts.asp?TID=8080>, <https://forum.asrock.com/forum_posts.asp?TID=8472>) | unverified for J4125B-ITX, must test in Phase 0 |
| Price | ~$113-$130 USD, board only | Beach Audio / Walmart listings, stock fluctuates | sourced, price only, availability unverified today |

**If CSM is missing** (real risk, see table): GRUB2 in UEFI mode can
still hand a multiboot kernel control the same way it does in QEMU today
- the kernel does not call BIOS interrupts after boot, it uses the
framebuffer GRUB already set up. The dependency on "legacy beats UEFI"
is really a dependency on GRUB working at all on the board, not on CSM
itself. Phase 0 below is exactly the test that settles this.

**Fallback: ODROID-H2+** (Gemini Lake, Intel Pentium/Celeron, SBC form
factor, official page: <https://www.hardkernel.com/shop/odroid-h2/> -
marked DISCONTINUED by Hardkernel; secondhand/eBay only as of this
writing). Dual 2.5GbE via Realtek RTL8125B, ALC662 audio, on-board
mSATA/SATA. Falls back to this only if the ASRock board's GRUB boot
fails outright in Phase 0 - it is USB-only for keyboard/mouse, so it
needs xHCI+HID working before any input at all, a strictly harder first
step.

## Driver gap table

| Chip on the pick | Kernel has today | Roadmap item | Effort |
|---|---|---|---|
| Realtek RTL8111H (Gigabit NIC) | rtl8139 (10/100, different register set) and ne2k | "Wired internet on real PCs: an Intel e1000 driver next to rtl8139 and ne2k" (docs/roadmap.md, Our own computer) - RTL8111 needs its own r8169-family driver, a sibling to that item, not the same chip | Medium. Same DMA-ring shape as rtl8139, different chip IDs, descriptor format and PHY init. A few days for someone who already wrote the rtl8139 driver. |
| ASM1061 / on-SoC SATA (AHCI) | ATA PIO (no AHCI, no DMA rings) | "AHCI for real SATA disks, on top of the e1000 work above" (roadmap) | Large. AHCI is a different command model (NCQ, command lists in memory) from PIO byte-banging. This is the biggest single item before "a real disk" is true. |
| ALC892 (Intel HDA bus) | SB16 (ISA, QEMU-only) | "Sound on real PCs: an AC97 or Intel HD Audio driver next to the Sound Blaster one" (roadmap) | Medium-large. HDA is PCI, ring-buffer based, codec-negotiated - closer in shape to the NIC work than to SB16's port I/O. |
| PS/2 keyboard/mouse (rear I/O, if present) | PS/2 driver exists and works in QEMU | none needed if the pick's PS/2 ports are real | None, if verified in Phase 0. This is the reason for the pick. |
| USB 3.2 xHCI (rear ports, for a boot stick and later HID/storage) | None | "xHCI USB host controller. Everything below hangs off it." (roadmap, first item under Our own computer) | Large. xHCI is the most complex controller here: ring-based TRBs, device enumeration, class drivers on top (HID, mass storage). Unavoidable long-term even with PS/2 covering input, because the boot stick itself is USB. |
| Intel UHD Graphics 600 (framebuffer) | Multiboot/VBE linear framebuffer, chipset-agnostic | none - GRUB sets the mode, kernel just writes pixels | None expected. This is the one piece that should just work, because GRUB (not the kernel) talks to the GPU. Phase 1 gate below proves it. |

## Bring-up order

Each phase ends in something you can point a camera or a log file at.
Do them in this order; do not start a phase until the previous one's
gate is met.

**Phase 0 - it boots at all.**
Flash the ISO to a USB stick, boot the board, and get *any* output: a
GRUB menu on screen, or a boot log over a serial cable if the screen
stays black. This is also where the CSM question above gets answered
for real.
Gate: a photo of the GRUB menu on the board's monitor, or a captured
serial log showing `kmain` starting.

**Phase 1 - the framebuffer.**
Same multiboot/VBE path QEMU already uses; the difference is whether
this specific Intel UHD 600 + GRUB combination hands back a usable
linear framebuffer address and mode.
Gate: the desktop wallpaper or shell prompt visible on the board's own
monitor, photographed.

**Phase 2 - input.**
PS/2 keyboard and mouse, if the port table above holds up. Type in the
shell, move a window.
Gate: a photo or short video of a command typed and executed on the
board.

**Phase 3 - storage.**
ATA PIO first against a real SATA disk in IDE-compat/legacy mode if the
board's BIOS offers it (many boards still do, even without full CSM) -
this proves "a real disk" without waiting on AHCI. AHCI work lands after,
as its own item.
Gate: `check.sh`-equivalent file write/read round-trip on the board,
logged.

**Phase 4 - network.**
RTL8111H driver up, DHCP or a fixed IP, a ping out.
Gate: a captured `ping` or an HTTP fetch from Samantha's chat, screen
recorded.

**Phase 5 - sound.**
HDA driver, the existing 1.6.9 audio path re-pointed at it instead of
SB16.
Gate: audio recording of the board playing back Samantha's voice or the
boot chime.

Each gate should be a file checked into the repo (photo, log, or a
`tools/checks/*.py` script's output), same discipline as every other
roadmap item here.

## Test rig

Iterating on real hardware without reflashing a USB stick every time is
the actual blocker, not any single driver. Plan:

- **Serial log is the priority over video.** A USB-to-TTL serial adapter
  (~$8-10, any FTDI-chip one) into the board's header if it has one, or a
  PCIe/USB serial card if not, gives a boot log without a monitor. This
  is how Phase 0 gets debugged without guessing from a black screen.
- **USB boot loop.** Keep two USB sticks: one known-good (last verified
  ISO) to fall back to, one for the build under test. `make iso` already
  produces the image; the loop is copy-to-stick, boot, note the result,
  repeat.
- **A board-side check script.** Once Phase 3 (storage) lands, a small
  `tools/checks/hwcheck.sh`-style script that runs from the board itself
  and writes its pass/fail to disk, so results survive a reboot instead
  of living only in someone's head. Not built yet - first real item once
  the board is in hand.
- **Network log, once Phase 4 lands.** After the NIC driver works, the
  kernel can push its own boot log to a listener on the LAN, which beats
  serial for convenience (no cable to the desk) but can't replace it for
  debugging Phase 0-3, when there's no network yet.

## Dev kit v0 BOM

Board-only prices, USD, gathered 2026-09-28. Stock and price move; treat
this as a snapshot, not a quote.

| Part | Pick | Price | Source |
|---|---|---|---|
| Motherboard + CPU | ASRock J4125B-ITX | ~$120 | Walmart/Beach Audio listings (estimate range $113-$130, stock fluctuates) |
| RAM | 8GB DDR4 SO-DIMM | ~$25 | generic current market price, estimate |
| Storage | 128GB SATA SSD | ~$20 | generic current market price, estimate |
| Case (mini-ITX) | generic mini-ITX case with a slot for the board's rear I/O | ~$40 | estimate |
| PSU | picoPSU or mini-ITX-compatible internal PSU | ~$30 | estimate |
| USB stick (ships pre-flashed with the ISO) | 16GB | ~$8 | estimate |
| Cables/misc (SATA cable, screws) | - | ~$7 | estimate |
| **Total (parts)** | | **~$250** | mostly estimate, board price is the only line with a real listing behind it |

This is a build-it-yourself BOM, not the $199 dev kit price - it is
already above $199 before labor, assembly, packaging and shipping are
added. MONEY.md resolves it with two boxes: a $199 Strata Kit (case and OS
stick, you bring the board) and a $349 Strata Complete.

## Risks and unknowns

- **CSM/legacy BIOS on the J4125B-ITX is unverified.** Related Gemini
  Lake boards (J4105-ITX, J5005-ITX) have user reports of no working
  CSM/legacy boot mode. If GRUB still boots the kernel in pure UEFI mode
  (likely, per the note above), this may not matter - but it is
  Phase 0's whole job to find out on the real board, not on paper.
- **Board availability.** J4125B-ITX shows out-of-stock at multiple
  retailers as of this writing. The dev kit BOM assumes it is buyable;
  if it goes fully EOL before Joshua orders one, the fallback (ODROID-H2+,
  also discontinued, secondhand-only) is worse, not better. A third
  option may be needed and isn't picked yet.
- **BOM total exceeds the $199 price** before any margin, as shown
  above. Needs either a cheaper board, volume discounts, or a price
  change - not resolved on this page.
- **Needs Joshua:** buy one J4125B-ITX board (or confirm a better
  current option), a case, RAM, an SSD, a USB-serial adapter, and a
  monitor/keyboard/mouse to test with. Nothing in Phase 0-2 can start
  without the physical board in hand.
- **PS/2 port claim is carried over from the plain J4125-ITX manual**,
  not confirmed against the B-revision's own manual PDF. Worth a five
  minute check against ASRock's official J4125B-ITX manual before
  ordering, since it's the whole reason for this pick.

## Enclosure: Strata

![Strata, front](hardware/strata-hero.jpg)

Strata is a stack of six rings, terracotta at the base fading to cream at
the top, like the layered rock around Joshua Tree. The 2 mm gaps between
rings are the vents. The tree mark is laser engraved half a millimetre
into the cap, tone on tone, so you see it up close and not across the
room. It's the concept for the custom shell; v0 still ships in a stock
mini-ITX case.

![Strata, rear I/O](hardware/strata-rear.jpg)

### The drawing

![Strata blueprint sheet](hardware/strata-blueprint.svg)

200 x 200 x 55 mm outside (a Mac mini is 127 x 127 x 50; the 170 mm board and its 158.75 mm I/O shield set the floor). Inside is a 178 mm core tray with 2 mm
walls and chamfered corners, which leaves a 174 mm cavity for the 170 mm board. The rear notch
cuts through every ring down to the tray so the stock 158.75 x 44.45 mm
I/O shield fits.

### Parts

Everything is split to fit a 180 x 180 x 180 mm bed (Bambu A1 mini). All numbers come from `hardware/strata_cad.py`, which fails if any part doesn't fit.

<!-- parts:start -->
| Part | Size | Print |
|---|---|---|
| Rings S0 to S5 | 200 mm square, 6.5 / 7.1 / 7.1 / 7.1 / 7.1 / 7.1 mm thick (base ring first, then equal strata), 2.0 mm vent gap between each, 24 quarters with cuts at +-30 mm alternating, three 24.0 x 2.4 mm spacer pads on the underside of rings 1 to 5, 8.4 mm behind the face, nut pocket under ring 0 and on top of ring 5 | FDM PLA or PETG at 0.2 mm, one tone each |
| Cap | 200 mm square frame in 4 pieces (cuts through the centre, never on a ring cut) and a 178 mm panel in one piece carrying the whole tree, 3 mm thick, panel and frame hole corners R2, tree from `landing/logo.svg` engraved 0.8 mm | Same print, printed right side up |
| Core tray | 178 mm square, 52.0 mm tall, 2 mm walls, corners chamfered 4.2 mm, vent slots on every gap line, 4 board standoffs printed on the floor, no rear wall | Printed, floor down, no supports |
| Rear plate | 173.6 x 51.1 mm and 13 mm deep including the facade, one piece, with the 160 x 45 I/O window and a 1.6 mm frame on all four sides, flush with the ring faces | Printed lying flat |
| Feet | 7.6 mm round, 1.6 mm thick, recessed under ring 0, cover the nut and the rod end | TPU or PLA |
<!-- parts:end -->

Hardware: 4 M3x50 rods, 8 M3 nuts, 4 M3x8 self-tapping screws (16 fasteners, was 20), all off the shelf.

35 printed pieces in 32 STL files (24 ring pieces, 5 cap pieces, tray, rear plate, 4 feet), down from 58 loose pieces. The 24 loose spacers and the metal standoffs are gone; the spacers and standoffs are printed into parts you already make, and four recessed feet hide the bottom nuts. Picture version: `hardware/BUILD.md`.

Full STL list with counts: `hardware/stl/manifest.json`. Print settings and steps: `hardware/ASSEMBLY.md`.

### Build it

Follow `hardware/ASSEMBLY.md`. The short version: rods and nuts in ring 0, tray in, rings, board on the printed standoffs, rear plate, nuts in ring 5, cap on top.

### Check before cutting

- Board mounting holes against the mini-ITX spacing (154.94 x 157.48 mm)
  on the real J4125B-ITX. The CAD file uses the spec numbers.
- Tallest part on the board, heatsink included, against the 62 mm tray.
- Power input type, from the board manual, and where its jack lands on
  the rear I/O.
- Passive cooling is fine on paper for this SoC; measure it in the stack
  under load before calling it done.
- No real print quote yet. The prototype cost below is an estimate; replace it with the JLC3DP cart total once the STLs are uploaded.

### Regenerate

Every file here comes from one set of numbers in `hardware/strata_cad.py`.

```sh
uv run --with build123d python docs/hardware/strata_cad.py docs/hardware   # STEP, STLs, step diagrams
python3 docs/hardware/blueprint_sheet.py docs/hardware/strata-blueprint.svg
blender -b -P docs/hardware/concepts.py -- docs/hardware strata  # renders
```

`strata_cad.py` asserts, in code, that:

- every printed part fits the 180 mm bed (the tray is the biggest at 178 mm)
- no flat roof is wider than 10 mm, so nothing needs supports (the tall rear window is its own plate, printed flat)
- every wall is 1.6 mm or thicker, every mating face has 0.2 mm of clearance
- the I/O window clears the 158.75 x 44.45 mm shield, the mounting holes match the 154.94 x 157.48 mm mini-ITX pattern, and the tray clears a 30 mm heatsink
- the rods clear the core and keep 1.6 mm of wall in the smallest ring

These caught real mistakes. The first draft's rods didn't fit the top rings. Its tray window was 158 mm, narrower than the 158.75 mm shield. The rear window was a 160 mm unsupported bridge. Rods had nothing to anchor to at the bottom, so v2 anchors them with nut pockets under ring 0, and the feet now close those pockets from below. Spacers are now pads on the rings, set 8.4 mm behind the face so the gaps read as vents.

## Prototype cost

Print it once before anyone is charged. All of this is an **estimate**, no quote yet. The STLs total about 900 cm3 solid, so with walls and 15 to 20% infill it's roughly 450 g of plastic.

| Route | Parts | Shipping | Total | Source |
|---|---|---|---|---|
| JLC3DP, PLA or resin, ugly gray | ~$25-45 | ~$15-30 | **~$40-75** | estimate, replace with the cart total |
| Bambu A1 mini at home | ~$9 plastic (450 g at ~$20/kg) | none | **~$9 after the $299 printer** | estimate |
| Hardware bag (rods, nuts, standoffs, screws) | ~$7 | included | **~$7** | same estimate as the existing BOM "cables/misc" line |

The existing dev kit BOM above is unchanged: its numbers still come from the listings and estimates stated there.

## Ordering from JLC3DP

1. Go to jlc3dp.com, pick 3D Printing, upload the STLs from `hardware/stl/`. Upload each file once and set the quantity from `manifest.json` (every ring quarter is 1, the cap frame pieces are 1 each, the feet are 4).
2. Material: PLA or PETG-like FDM if offered, otherwise the cheapest resin (Standard Resin, gray). Layer 0.1 to 0.2 mm. Color: one per ring, hex values in `hardware/BUILD.md` and `hardware/stl/manifest.json` (terracotta B9542C at the base to cream F0E7D8 at the cap, black for tray, rear plate and feet). A single grey order has the right shape and no fade.
3. Quantity per file: from the manifest.
4. Open the DFM preview. Check: every part lies flat as uploaded, no wall flagged under 1.6 mm, the quarters aren't merged into one body, the rear plate window and the nut pockets show as holes. If it flags the rear ring quarters, which are cut away for the I/O plate, ask for PETG or accept the risk, the thinnest strip is 4.7 mm tall and stiff once stacked.
5. Look at the cart total. Write the real number into this page and into MONEY.md in place of the estimate. That one number decides the kit margin.
6. Nothing is ordered until Joshua says so.
