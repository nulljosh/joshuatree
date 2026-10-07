# Design

This is the one page of rules for how Joshua Tree looks and what it never does. Every number in a code span below is read back from the source by `tools/checks/design-doc-check.py`, so when a rule drifts the suite says so.

## What the OS never does

It never shows a pixel. The desktop is laid out at `DESKTOP = 960x540` and drawn at `PIXEL_SCALE = 2` times that. Text, icons and the wallpaper are written on the real pixels and anti-aliased. Plain rectangles are 2x2 blocks, which is fine because they have no diagonal edges.

It never draws an emoji. UI text covers `UI_GLYPH_RANGE = 32-126`, plain ASCII, plus the degree sign. It never shows purple or teal in an icon or in the window chrome, and never puts a border, stripe or card edge on something just to decorate it. Super simple wins: one window frame, one dock, one light.

## Icons

Every icon is one picture on one tile. The tile is a superellipse, `SQUIRCLE_N = 4.8`, over the whole 128 unit canvas, and an icon's edge must land within `TOL_SQUIRCLE = 1.0` unit of that curve at every angle, with a soft anti-aliased edge. A plain rounded rectangle fails.

The glyph is bold and sits in the middle. It keeps out of the top `GLYPH_TOP_MARGIN = 16` and bottom `GLYPH_BOTTOM_MARGIN = 20` units, because that band belongs to the tile's own light.

There is one light, from the top. A dock tile gets darker from top to bottom by `DEPTH_MIN = 6` to `DEPTH_MAX = 45` luminance steps, never a dark rim (`EDGE_MAX = 6`), and a white lip along the top edge, stroked `HL_WIDTH = 5.0` units wide, `HL_ALPHA = 0.5` strong, fading out over `HL_FADE = 22` units. Gradients exist only to carry that light, so they never change hue: none runs more than `GRADIENT_HUE_MAX = 15` degrees of hue from end to end.

No text, letters or wordmarks, so `ICON_TEXT_ELEMENTS = 0`. The one exception is the Calendar tile: its art is a blank tile and the kernel writes today's month and day on it, because a calendar showing a fixed date would lie. No emblem and no speech bubble drawn around a picture. No purple or teal in any icon's colours, so `ICON_PURPLE_TEAL = 0`.

The house accent is terracotta, `ACCENT = #b5502c`, the landing page's accent and the colour of the Music, Movies and Hamurapi tiles. The one deliberate exception is Tonchi, which keeps its own sky blue, `LEXLY_BLUE = #2E86DE`, because that is the colour of its store icon.

Fleet icons are the `FLEET_ICONS = 10` apps imported from their own repos. They keep their own tile colour and their own picture. They must still share the squircle, the anti-aliased edge, the top lip and the glyph margin. They do not get the tile falloff, so a fleet tile is flat. The importer clips each one to the squircle and lays the lip on it. The checks measure the squircle and the edge on all `ICON_FILES = 33` icons, but the glyph margin only on the `MARGIN_TILES = 20` that carry the shared tile header written by `restyle_icons.py`. A fleet icon's margin is held by eye, and the roadmap lists the fleet icons that still fall short.

## Type

The registry holds `DEJAVU_FACES = 14` faces. Six are DejaVu: Sans, Sans Bold, Serif, Serif Bold, Mono and Mono Bold. Eight are open look-alikes for documents and user choice: Inter, EB Garamond, Comic Neue, Caveat Brush, Dancing Script, JetBrains Mono, Source Serif and Source Sans (`fonts` lists them). Sans only in the UI chrome: the menu bar, dock and window titles stay DejaVu Sans. Sans is the interface. The desktop draws it at `GUI_TEXT_PX = 24` physical pixels, 12 on the 960 wide grid. Apps draw it at `BODY_PX = 14.0`, with bold at the same size and the big display digits at `DISP_PX = 56.0`.

On the desktop, Mono is for a shell grid and nothing else: `MONO_APPS = panes, terminal`, at `MONO_PX = 13.5` with every glyph `JT_MONO_ADV = 8` pixels wide. Serif ships with the fonts and nothing draws with it, so `SERIF_USES = 0`. Sans-serif only is the house rule, so serif has to earn its way in.

The retina rule holds for type as it does for icons: stems are inked dense but stay anti-aliased. The 8x16 bitmap font is only the fallback for VGA text mode and for off-screen targets.

## Colour and light

The wallpaper is the satellite photo in colour, `DEFAULT_WALLPAPER = WALL_SAT`, live tiles when there is a network and the photo baked into the kernel when there is not. It is never turned black and white, and the tree photo is a choice in Settings, never the boot default.

Day and night are a blend on the wallpaper, read from the same clock as the menu bar, and they never go toward black or blue. From `NIGHT_HOURS = 21-5` the wallpaper sits `NIGHT_PCT = 65` percent of the way to the espresso brown `NIGHT_FLOOR = 0x00201009`, easing in over the evening and out over the morning. In `DAY_HOURS = 8-18` it lifts `DAY_PCT = 10` percent toward silver, `DAY_TINT = 0x00DDDDDD`.

The menu bar is `GUI_MENUBAR_H = 26` pixels tall, half wallpaper and half white (`MENUBAR_WHITE = 50` percent), closed by a one pixel rule, `MENUBAR_RULE = 0x00BDB8B0`. The dock tray is warm cream, `DOCK_TRAY_COLOR = 0x00EFEBE4`. A window body is `WINDOW_BODY = 0x00F5F0EB` with ink `WINDOW_INK = 0x001C1C1E`.

## Windows and dock

A window is one frame: a body with a `WINDOW_RADIUS = 18` pixel corner, a one pixel rule `WINDOW_RULE = 0x00D9D3CB` under the title band, the name centred in ink, and a red close dot, `CLOSE_DOT = 0x00FF5F57`, at the left. The few full-screen panels (Settings, Trash, the lock screen) use a plainer bar with the same dots. The edge and the corner are one anti-aliased curve, with no notch where they meet.

Drag a window by its title bar. It follows the pointer live. Let go within `SNAP_EDGE = 12` pixels of a side and it takes that half of the strip between the menu bar and the dock; within `SNAP_CORNER = 40` pixels of a corner it takes that quarter; at the top edge it takes the whole strip.

The dock holds `DOCK_SLOTS = 11` tiles in this order: `DOCK_ORDER = Apps, Burrow, Mail, Calendar, Notes, Reminders, Terminal, Samantha, Weather, Stocks, Trash`. Portfolio mode swaps in the fleet. A tile is `dock_scale_pct = 7` percent of the screen height, so `DOCK_ICON = 37` pixels on the desktop, with `DOCK_GAP = 6` between tiles, `DOCK_PAD = 10` inside the tray and `DOCK_MARGIN_BOT = 24` below it. The tray has a `TRAY_RADIUS = 18` corner, a hairline `TRAY_EDGE = 0x00D6D0C6` top and bottom, and a soft shadow `TRAY_SHADOW_ROWS = 10` rows deep that follows the wallpaper. Hover shows the app's name in a pale capsule, `DOCK_LABEL_BG = 0x00F4F1EC` with a `DOCK_LABEL_EDGE = 0x00BDB4A8` hairline. Nothing magnifies or bounces.

The Launchpad is `APPS_COLS = 5` columns of icons at most `APPS_TILE = 56` pixels. Its glass panel is centred, at least `LAUNCHPAD_MIN_TOP = 24` pixels from the menu bar and the dock and `LAUNCHPAD_MIN_SIDE = 40` from the sides, the padding is even, and no two labels are closer than `LABEL_MIN_GAP = 6` pixels.

## Motion

The shell barely moves. Windows do not animate open, close or snap. The dock only shows its label. What moves is Samantha's captions, on a clock of `TICK_HZ = 100` ticks a second. A new line rises into place over `CAP_IN = 0.28 s`, easing out as a square (`CAP_EASE = quadratic out`). Up to `CAP_SHOW = 3` lines show at once, each older one dimmer. When the talk has been quiet for `CAP_QUIET = 4 s` the whole stack fades together over `CAP_OUT = 0.6 s`. Her caption backdrop is black at `CAP_ALPHA = 150` out of 256.

Apps move only when the movement is the app: Windgate's breathing circle, Samantha's mouth. The tree photo wallpaper sways in the wind; the satellite default stands still.

## How it is enforced

The numbers above are checked by `design-doc-check.py` against the file that defines each one. It also reads every icon in `art/icons` for text, purple, teal and gradient hue.

Run in the suite on every pull request: `iconinset-check.py` (the squircle and the edges on every icon, the glyph margin on the tiles with the shared header), `iconlight-check.py`, `iconhalo-check.py`, `iconedge-check.py` (its only hard assert is the Trash rim, the rest is a score to read), `calicon-check.py`, `clockicon-check.py`, `dockslots-check.py`, `dockband-check.py`, `dockhover-check.py`, `windowedge-check.py`, `windowsnap-check.py`, `windowdrag-check.py`, `launchpad-centered-check.py`, `textsharp-check.py`, `textspacing-check.sh` and `termsharp-check.py`.

Run by hand, not in CI yet: `iconart-check.py`, `iconsync-check.sh` and `dockshadow-check.py`.

Not measured, held by eye: a bold glyph, one picture per icon, no emblem or speech bubble, no stripes or decorative borders, and the glyph margin of the icons without the shared tile header (the fleet icons, Movies, Panes and Portfolio).
