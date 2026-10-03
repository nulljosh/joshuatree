# Strata Pi: a printable case for the Raspberry Pi 4B

The same stacked-ring look as the [Strata enclosure](../HARDWARE.md#enclosure-strata), sized for the Pi. Four rings that get smaller as they climb, a cap, and a tray in the middle that holds the board. Every part fits a 180 mm printer bed (a Bambu A1 mini is enough).

![Front elevation](strata-pi-front.svg)

Overall size is about 131 x 102 x 27.5 mm.

## Print the fit test first

The port positions come from the Raspberry Pi 4B mechanical drawing, and they have not been checked against your board yet. So print one small part before the rest. It takes about ten minutes.

1. Build the files (below).
2. Print `fit_test.stl`. It is a flat plate with a thin lip the size of the board and four holes.
3. Drop the Pi in. The board should sit inside the lip and the four mounting holes should line up with the plate's holes.
4. If it is off by half a millimetre or more, change `BOARD_L`, `BOARD_S` or `HOLES` at the top of `strata_pi_cad.py` and rebuild.

The port windows are cut bigger than the connectors on purpose, so small errors still fit. The mounting holes are the part that has to be right.

## Build the files

```
uv run --with build123d python docs/hardware/strata_pi_cad.py docs/hardware/pi
```

That writes one STL per part, the whole thing as a STEP file, and two drawings. The script stops with an error if the rods would miss the rings, the cap would not cover the tray, or the tallest port would hit the lid.

| File | What it is | Qty |
|---|---|---|
| `core_tray.stl` | Floor, walls, four standoffs, port windows, vent slots | 1 |
| `stratum0.stl` to `stratum3.stl` | The four rings, biggest at the bottom | 1 each |
| `cap.stl` | The lid | 1 |
| `fit_test.stl` | The board outline test plate | 1, first |

## What you need besides the prints

- Four M3 bolts, about 40 mm long, four M3 nuts (or threaded rod cut to length). They run through the corners of every ring and the cap.
- Four M2.5 screws, 6 mm. They hold the Pi to the standoffs. The standoffs are printed and the screws cut their own thread.

## Print settings

- PLA is fine. PETG if the case will sit somewhere warm.
- 0.2 mm layers, 3 wall loops, 20% infill.
- No supports. Lay every part flat on its biggest face. The tray prints floor down.
- Colour: the Strata look runs a dark orange at the bottom to cream at the top. The colours are in [HARDWARE.md](../HARDWARE.md).

## Assemble

1. Screw the Pi onto the four standoffs in the tray. The USB-C and HDMI ports face the front window, and the USB and Ethernet ports face the right window.
2. Slide the tray into the biggest ring.
3. Stack the other three rings and the cap. Run the four bolts down through the corners and tighten the nuts under the base ring.
4. The gaps between rings are the vents. Leave them open.

## What the windows are for

| Side | What shows through |
|---|---|
| Front | USB-C power, both micro-HDMI ports, the 3.5 mm jack |
| Right | Ethernet and all four USB ports |
| Back | The 40-pin header, so a serial cable or a HAT still fits with the case closed |
| Left | The microSD card |

The back window is the one you need on day one. The serial cable plugs into the header while the lid is on.

## Not done yet

- No print has been made. The numbers come from the drawing, so treat the first print as a test.
- No Pi 5 version. The Pi 5 board is the same size but the ports are laid out differently.
- No fan. The Pi 4 runs warm under load. The vent gaps are enough for the idle work Joshua Tree does today. A fan mount comes if M3 runs hot.
