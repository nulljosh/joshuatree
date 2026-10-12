# Samantha box: a desk unit for the Pi

A 7 inch screen, a Raspberry Pi 4B, a round speaker and a mic hole in one printable box. Samantha lives in it.

![The box, exploded, from the front](samantha-box-front.jpg)

About 171 x 124 x 34 mm. Two parts make the box. Two small cradles lean it back on the desk.

| File | What it is |
|---|---|
| `back_shell.stl` | Floor, four walls, four screw bosses, Pi standoffs, a ring for the speaker, a grille in the back, port windows, the mic hole |
| `front_bezel.stl` | A frame with a pocket for the screen, a window for the lit area, and four M3 holes with the heads sunk below the face |
| `stand.stl` | Two cradles, one under each end, with a slot cut 15 degrees off upright. The box drops in with half a millimetre each side |
| `fit_test.stl` | A thin tray with the screen's pocket in it. Print it first |
| `samantha-box.step` | The shell and the bezel only, for any CAD program. The stand and the fit test are STL only |

Four M3 x 10 socket head screws hold the bezel to the shell.

## Nothing here is measured yet

Every part number at the top of `samantha_cad.py` (the screen, the speaker, the power plug) is a stand-in. Nothing has been printed.

Print `fit_test.stl` first. It is a frame on a ledge, open in the middle so the screen's own connectors hang through. Drop the real screen in. It should sit flush with the top and move a hair side to side. Tight or loose: fix `SCREEN_W` and `SCREEN_H`. Proud or sunk: fix `SCREEN_T`. The notch on the top edge is for a finger to lift it out. Then measure the speaker's diameter and height. The Pi numbers live in `pi4b.py`, shared with the [Neo Pi case](PI-CASE.md).

```
uv run --with build123d python3 docs/hardware/samantha_cad.py /tmp/box
```

The script checks every fit before it writes a file, and stops with the reason when a change breaks one:

- a screw boss runs into the Pi, a port window, the card hole, the speaker or the screen
- a bezel screw lands on the screen
- a grille bar gets too thin to print, or a piece of the shell comes loose
- the screen stops dropping into the bezel or the fit test
- the box stops dropping into the stand, the stand hides part of the screen, or the power plug has no room under the box

`tools/checks/samantha-box-check.py` runs it in the suite wherever uv and build123d are installed.

## How it is laid out

The Pi sits against the right and bottom walls, so its USB, Ethernet, power and HDMI ports meet the walls through windows. Above and below the screen there is a 9 mm band for the four M3 screws, so none lands on the panel. Three bosses sit in the corners. The Pi fills the fourth corner, so that boss sits on the bottom band to its left.

The speaker sits on the left and fires out of the back through a grille. A gap is left beside the microSD slot, with a hole in the back under it, so the card can be changed without opening the box. The mic hole is in the top wall.

![The back: the speaker grille, the card hole and the USB window](samantha-box-back.jpg)

The power, HDMI and audio ports face down. The cradles only hold the ends of the box, so the power lead reaches the Pi from below, between them. It needs a right-angle USB-C plug that stands no more than 12 mm off the box (`PLUG_H`).

## Not done yet

- Nothing holds the screen from behind. It rests in the bezel's pocket, so a firm press on a touch panel could push it in. Ledges on the shell, or the panel's own mounting holes, come once the real panel is measured.
- The screen's own HDMI and touch connectors stick out of its back. Their height and place are unknown, so the clearance to the Pi is unchecked.
- No cable route from the Pi's micro-HDMI to the screen.
- No mic part is chosen. Joshua's Yeti is too big for this box.
- No fan. See [PI-CASE.md](PI-CASE.md).

The renders come from `python3 docs/hardware/render_samantha.py`. It builds the CAD into a temp folder, renders both views in Blender with no window, and writes the two JPGs here.
