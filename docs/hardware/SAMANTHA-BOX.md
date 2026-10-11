# Samantha box: a desk unit for the Pi

A 7 inch screen, a Raspberry Pi 4B, a round speaker and a mic hole in one printable box. Samantha lives in it.

![The box, exploded, from the front](samantha-box-front.jpg)

About 171 x 106 x 34 mm. Two parts make the box and a third makes it lean back.

| File | What it is |
|---|---|
| `back_shell.stl` | Floor, four walls, screw bosses, Pi standoffs, a ring for the speaker, a grille in the back, port windows, the mic hole |
| `front_bezel.stl` | A frame with a pocket for the screen and a window for the lit area |
| `stand.stl` | A block with a slot cut 15 degrees off upright. The box's bottom edge drops in |
| `fit_test.stl` | A flat frame the size of the screen. Print it first |

## Nothing here is measured yet

Every part number at the top of `samantha_cad.py` (screen, board, speaker) is a stand-in. Nothing has been printed. Print `fit_test.stl` first (about ten minutes), lay the real screen in it, and fix `SCREEN_W`, `SCREEN_H` and `SCREEN_T` until it sits flat with a hair of play. Then do the same with the speaker's diameter and height. The Pi numbers are the Neo Pi case's.

```
uv run --with build123d python docs/hardware/samantha_cad.py /tmp/box
```

The script stops with an error when a change would stop the Pi's tallest port clearing the screen, push the Pi through the wall, crowd the speaker over the card, or cut the stand's floor away.

## How it is laid out

The Pi sits against the right and bottom walls, so its USB, Ethernet, power and HDMI ports meet the walls through windows. The speaker sits on the left and fires out of the back through a grille. A gap is left beside the microSD slot, with a hole in the back under it, so the card can be changed without opening the box. The mic hole is in the top wall.

## Not done yet

- The screen's own HDMI and touch connectors stick out of its back. Their height and place are unknown, so the clearance to the Pi is unchecked.
- No cable route from the Pi's micro-HDMI to the screen.
- No mic part is chosen. Joshua's Yeti is too big for this box.
- No fan. See [PI-CASE.md](PI-CASE.md).
- The renders come from `tools/gen/samantha_box_render.py`: `blender -b --factory-startup -P tools/gen/samantha_box_render.py -- /tmp/box docs/hardware`, then shrink them.
