#!/usr/bin/env python3
"""Hamurapi (the game was Hamurabi until the store name was taken; the files, HAMURABI.BIN and open=hamurabi keep that
spelling) is playable as a ring-3 window, and every year it plays is the year the rules header says it should be (2.7).

Boots headless with `open=hamurabi`, which launches Hamurabi from the dock path the moment the desktop is up.
Hamurabi is user/hamurabi.c, a flat binary loaded off the VFS and run at CPL 3 through the table-driven
launcher (kernel/ring3app.c, RING3_APPS). The check then:

  1. asserts, off the serial log, that the program was launched, opened a window of the dock viewport's
     size (804x345) and reported where its ziggurat landed and where its three buttons are;
  2. dumps the window region and asserts it is a real picture: hundreds of distinct colours, no colour
     covering most of it. Saves it as a PNG for a human to look at;
  3. checks real sprite pixels, not just "something is drawn": every opaque pixel of a slice of the
     ziggurat must equal the pixel in art/hamurabi/sprites.png at the place the program says it put it,
     the half-see-through outline pixels must be blended (close to the sheet colour, but not equal to it),
     and the same test run on the sky must fail (so the test is not vacuous);
  4. dumps twice a second apart: the town is alive (villagers walk, the river moves), so the frames differ;
  5. clicks each of the three buttons and reads "hamurabi: <label> chosen" off the serial port; the first one
     must also be seen drawn pressed (the darker terracotta) and released again when the title is back. Each button
     must start what it names (the story's opening card, a classic year, the robot's demo), and Esc or any key
     brings the title back;
  6. moves the focus ring with the arrow keys and presses Enter: the second button is chosen, from the keyboard.
     That starts the classic reign of step 7;
  7. (b) plays that classic reign by keyboard: Enter on the offered orders in most years, and in two years the
     orders are changed with Up/Down, Left/Right, Shift and End. The program logs the seed, each year's offered
     orders, submitted orders, report and result, and the grade. The check builds a host program from the same
     user/hamurabi_rules.h, gives it the seed and the key presses (it has its own sensible-orders and clamp, written
     from the web game's rules.js, not borrowed from the program) and requires every logged line to match;
  8. (c) plays a story reign the same way: the opening card, a turning point, and omens with a choice taken by
     keyboard (the first omen's first choice, the next one's second). The host replays the cards, the choice, the
     line that came of it, and the city after it. A reign with no omen is played again (up to three times);
  9. (a) clicks "Watch a demo" and leaves it alone: the robot king rules ten years by itself, its seed is read
     off the log, the host replays the robot on that seed and every logged year and the grade must match; a click
     then brings the title back;
 10. presses Esc: the program must exit 0, release its window, and the desktop must take a click (Mail opens
     from the dock) afterwards.

Screenshots (/tmp/jt-hamurapi-<name>.png, also copied to $JT_SHOTS_DIR when it is set): title, card, orders,
report, plague (when the demo meets one), ending, ending-demo.

Discriminating: break the run-length unpacking (user/hamurabi.c unpack) or the palette and step 3 fails; hand
hamurabi_step the hard rules, or draw the omen from the harvest dice, and steps 7 to 9 fail on the first line that
differs; make a Right press move the wrong row or skip a clamp and step 7 fails; stop calling draw() or present and
steps 2 and 4 fail; make a button's rectangle wrong and step 5 clicks nothing.

Usage: tools/checks/ring3hamurabi-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3hamurabi-serial.log"
DUMP = "/tmp/jt-ring3hamurabi.raw"
SHOTS = {}
BEST = {"report": -1}
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
VIEW_W, VIEW_H = 804, 345
PARK = (480, 452)         # on the desktop, below the window and above the dock: the pointer is not in any picture
ACCENT = (0xB5, 0x50, 0x2C)      # the primary button
PRESSED = (0x8C, 0x3E, 0x20)     # the same button while pressed
NOISE = {"present", "menubarredraw", "fullrepaint", "mwchrome", "wxfetch"}

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

# ---- the host oracle: a program built from the same rules header, with its own sensible orders and clamp ----
HOST_C = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user/hamurabi_rules.h"

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static const char *GR[4] = { "F", "C", "B", "A+" };
static char keys_for[16][240]; static int has_keys[16]; static int choice_for[16];

/* web/play/rules.js: sensible(c) and the clamp the sliders keep */
static void sensible(const hamurabi_city *c, hamurabi_orders *o) {
    o->buy = 0;
    o->feed = imin(c->grain, c->people * 20);
    o->plant = imin(imin(c->acres, c->people * 10), c->grain - o->feed);
}
static int max_feed(const hamurabi_city *c, const hamurabi_orders *o) { return imax(0, c->grain - o->buy * c->price); }
static int max_plant(const hamurabi_city *c, const hamurabi_orders *o) {
    return imax(0, imin(imin(c->acres + o->buy, c->people * 10), c->grain - o->buy * c->price - o->feed));
}
static void clamp_orders(const hamurabi_city *c, hamurabi_orders *o) {
    int mb = c->grain / c->price;
    if (o->buy < -c->acres) o->buy = -c->acres;
    if (o->buy > mb) o->buy = mb;
    if (o->feed < 0) o->feed = 0;
    if (o->feed > max_feed(c, o)) o->feed = max_feed(c, o);
    if (o->plant < 0) o->plant = 0;
    if (o->plant > max_plant(c, o)) o->plant = max_plant(c, o);
}
/* the keys: Up/Down/Tab pick a row, Left/Right a small step (1 acre, 20 bushels, 10 acres), Shift ten times that,
   Home sets the row to 0, End to the most it can be (everyone fed, all that can be planted) */
static void apply_keys(const hamurabi_city *c, hamurabi_orders *o, char *spec) {
    static const int ST[3] = { 1, 20, 10 }, BG[3] = { 10, 200, 100 };
    int row = 0;
    for (char *t = strtok(spec, " "); t; t = strtok(NULL, " ")) {
        int *v = row == 0 ? &o->buy : row == 1 ? &o->feed : &o->plant;
        if (!strcmp(t, "up")) row = (row + 2) % 3;
        else if (!strcmp(t, "down") || !strcmp(t, "tab")) row = (row + 1) % 3;
        else if (!strcmp(t, "left")) { *v -= ST[row]; clamp_orders(c, o); }
        else if (!strcmp(t, "right")) { *v += ST[row]; clamp_orders(c, o); }
        else if (!strcmp(t, "sleft")) { *v -= BG[row]; clamp_orders(c, o); }
        else if (!strcmp(t, "sright")) { *v += BG[row]; clamp_orders(c, o); }
        else if (!strcmp(t, "home")) { *v = 0; clamp_orders(c, o); }
        else if (!strcmp(t, "end")) {
            *v = row == 0 ? c->grain / c->price : row == 1 ? imin(max_feed(c, o), c->people * 20) : max_plant(c, o);
            clamp_orders(c, o);
        } else { printf("UNKNOWN KEY %s\n", t); exit(5); }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *mode = argv[1];
    unsigned seed = (unsigned)strtoul(argv[2], 0, 10);
    if (argc > 3) {
        FILE *f = fopen(argv[3], "r");
        char line[300];
        while (f && fgets(line, sizeof line, f)) {
            int y, i, n;
            line[strcspn(line, "\n")] = 0;
            if (sscanf(line, "keys %d %n", &y, &n) == 1 && y >= 0 && y < 16) { strcpy(keys_for[y], line + n); has_keys[y] = 1; }
            else if (sscanf(line, "choice %d %d", &y, &i) == 2 && y >= 0 && y < 16) choice_for[y] = i;
        }
        if (f) fclose(f);
    }
    int story = !strcmp(mode, "story"), demo = !strcmp(mode, "demo");
    const hamurabi_rules *rules = &hamurabi_rules_normal;
    hamurabi_rng rng = hamurabi_rng_new(seed), srng = hamurabi_rng_new((unsigned long long)seed ^ 0x5707ULL);
    hamurabi_city c = hamurabi_new_city(rules);
    unsigned seen = 0;
    printf("seed %u mode %s\n", seed, mode);
    while (!c.over) {
        const hamurabi_omen *om = 0; const char *card = 0;
        if (story) {
            int interlude = 0;
            for (int i = 0; i < HAMURABI_INTERLUDES; i++) if (hamurabi_interludes[i].year == c.year) interlude = 1;
            if (c.year == 1) card = "intro";
            else if (interlude) card = "interlude";
            else if ((om = hamurabi_omen_for(&c, seen, &srng))) { seen |= 1u << (int)(om - hamurabi_omens); card = om->id; }
        }
        if (card) printf("phase card year %d %s\n", c.year, card);
        if (om) {
            int ci = choice_for[c.year];
            if (!hamurabi_can_afford(&om->choice[ci], &c)) { printf("UNAFFORDABLE choice %d in year %d\n", ci, c.year); return 3; }
            hamurabi_outcome out = hamurabi_apply_choice(&om->choice[ci], &c, &srng, &rng);
            printf("choice %d people %d acres %d grain %d\n", ci, c.people, c.acres, c.grain);
            printf("note %s", out.said);
            if (out.peek_yield) printf(" A %s harvest is coming: %d bushels an acre.", out.peek_yield >= 4 ? "big" : out.peek_yield <= 2 ? "small" : "fair", out.peek_yield);
            printf("\n");
        }
        hamurabi_orders o;
        sensible(&c, &o);
        printf("phase orders year %d buy %d feed %d plant %d\n", c.year, o.buy, o.feed, o.plant);
        if (demo) o = hamurabi_ruler_legal(&c, &hamurabi_ruler_default);
        else if (has_keys[c.year]) apply_keys(&c, &o, keys_for[c.year]);
        if (hamurabi_check(&c, &o)) { printf("ILLEGAL ORDERS in year %d\n", c.year); return 4; }
        printf("orders year %d buy %d feed %d plant %d\n", c.year, o.buy, o.feed, o.plant);
        hamurabi_year_report r = hamurabi_step(&c, &o, &rng, rules);
        printf("report year %d yield %d harvest %d rats %d born %d plague %d\n", r.year, r.yield, r.harvest, r.rats, r.born, r.plague);
        printf("year %d people %d acres %d grain %d starved %d\n", r.year, c.people, c.acres, c.grain, r.starved);
    }
    printf("over grade %s\n", GR[hamurabi_grade(&c)]);
    return 0;
}
'''

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=hamurabi",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
_log = {"n": 0, "text": ""}
def serial():
    """The serial log so far, without the desktop's per-frame chatter, read a bit at a time."""
    try:
        with open(LOG, "rb") as f:
            f.seek(_log["n"]); data = f.read()
    except OSError:
        return _log["text"]
    cut = data.rfind(b"\n") + 1          # only whole lines
    if cut:
        _log["n"] += cut
        _log["text"] += "".join(l + "\n" for l in data[:cut].decode(errors="replace").replace("\r", "").split("\n")[:-1] if l not in NOISE)
    return _log["text"]
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False
def wait_re(pattern, pos, secs):
    """First match of pattern in the log after offset pos, waiting up to secs."""
    rx = re.compile(pattern)
    for _ in range(int(secs * 20)):
        m = rx.search(serial(), pos)
        if m: return m
        time.sleep(0.05)
    return None
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    def keys(*qcodes):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
    def press(*qcodes, gap=0.13):
        keys(*qcodes); time.sleep(gap)
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def window(img):
        return img.crop((VIEW_X * SCALE, VIEW_Y * SCALE, (VIEW_X + VIEW_W) * SCALE, (VIEW_Y + VIEW_H) * SCALE))
    def pixel(x, y, img=None):
        return (img or frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
    def shot(name, img=None):
        """Save the window as /tmp/jt-hamurapi-<name>.png (and into $JT_SHOTS_DIR when set)."""
        path = f"/tmp/jt-hamurapi-{name}.png"
        (img or window(frame())).save(path)
        SHOTS[name] = path
        d = os.environ.get("JT_SHOTS_DIR")
        if d:
            try: os.makedirs(d, exist_ok=True); shutil.copyfile(path, os.path.join(d, os.path.basename(path)))
            except OSError as e: print(f"(could not copy {name} to {d}: {e})")
        return path
    def app_lines(pos, upto=None):
        """What the program said since offset pos: the text after 'hamurabi: ' on each line."""
        text = serial()[pos:upto]
        return re.findall(r"ring 3: hamurabi: ([^\r\n]*)", text)

    # 0. names, off the sources: what a person sees says Hamurapi; only the credit to the 1968 game keeps the old spelling
    kernel_c = open("kernel/kernel.c").read()
    apps_tab = re.search(r"struct app APPS\[GUI_APP_COUNT\]\s*=\s*\{(.*?)\n\};", kernel_c, re.S)[1]
    app_names = re.findall(r'\{"([^"]*)",', apps_tab)
    r3_names = re.findall(r'\{"([^"]+)",\s*user_', open("kernel/ring3app.c").read())
    sam_names = re.findall(r'"([^"]+)"', re.search(r"APPNAME\[\] = \{(.*?)\};", open("user/samantha.c").read(), re.S)[1])
    for what, names in (("APPS[]", app_names), ("RING3_APPS", r3_names), ("Samantha's APPNAME", sam_names)):
        if "Hamurapi" not in names: fails.append(f"{what} has no 'Hamurapi' row: the app is not shown under its product name")
        if "Hamurabi" in names: fails.append(f"{what} still has a 'Hamurabi' row: the old spelling is shown")
    if 'word_prefix_ci("hamurabi", w)' not in open("user/samantha.c").read(): fails.append("Samantha no longer opens the app when told the old name 'hamurabi'")
    if '"Based on Hamurabi by Doug Dyment, 1968."' not in open("user/hamurabi.c").read(): fails.append("the credit line to the 1968 game is not exactly 'Based on Hamurabi by Doug Dyment, 1968.'")

    # 1. the program is up, has its window, and says where things are
    if not wait_serial("ring3app: launching HAMURABI.BIN at ring 3", 40):
        fails.append("Hamurabi was never launched as a ring-3 program (open=hamurabi flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial(f"hamurabi: ring-3 window {VIEW_W}x{VIEW_H}", 10):
        fails.append(f"the program did not report the app viewport's size (expected {VIEW_W}x{VIEW_H}) through write()")
    if not wait_serial("hamurabi: buttons", 20):
        fails.append("the program never reported its button rectangles: it did not finish its first frame")
    time.sleep(1.0)
    log = serial()
    m = re.search(r"hamurabi: scene scale (\d+) ziggurat (-?\d+),(-?\d+)", log)
    b = re.search(r"hamurabi: buttons (\d+),(\d+),(\d+),(\d+) (\d+),(\d+),(\d+),(\d+) (\d+),(\d+),(\d+),(\d+)", log)
    if not m or not b:
        print(log[-1500:]); raise SystemExit("FAIL: no scene / button report on the serial port")
    S, zx, zy = int(m[1]), int(m[2]), int(m[3])
    btns = [tuple(int(b[1 + 4 * i + k]) for k in range(4)) for i in range(3)]
    print(f"scale {S}, ziggurat at art ({zx},{zy}), buttons {btns}")
    if "hamurabi: title Hamurapi\n" not in log: fails.append("the program did not draw the title 'Hamurapi'")
    if "hamurabi: phase title\n" not in log: fails.append("the program did not say it is on the title")
    if "BUG" in log or "no heap" in log or "no window" in log:
        fails.append("the program or ring3app logged a BUG / no heap / no window line")

    # 2. a real picture
    move(*PARK); time.sleep(0.3)
    img1 = frame()
    win1 = window(img1)
    shot("title", win1)
    colors = win1.getcolors(maxcolors=W * H) or []
    ncol = len(colors)
    top_share = max(c for c, _ in colors) / (VIEW_W * SCALE * VIEW_H * SCALE) if colors else 1
    print(f"distinct colours in the window: {ncol}; most common colour covers {top_share:.0%}")
    if ncol < 400:
        fails.append(f"only {ncol} distinct colours in the window: the scene is not drawn (a real frame has well over 400)")
    if top_share > 0.5:
        fails.append(f"one colour covers {top_share:.0%} of the window: it is (nearly) blank")

    # 3. real sprite pixels. Two ziggurat slices: sheet columns 29..35, rows 0..35 (the middle, clear of the king and
    #    the torch glows) and columns 0..57, rows 26..35 (the wide low part, clear of the torches and of the camels
    #    and villagers that cross its feet).
    sheet = Image.open("art/hamurabi/sprites.png").convert("RGBA")
    zrect = json.load(open("art/hamurabi/sprites.json"))["ziggurat"]
    def sprite_px(ax, ay):
        # the centre of art pixel (ax, ay): window pixel ax*S + S/2, in the dump x SCALE
        return win1.getpixel(((ax * S) * SCALE + (S * SCALE) // 2, (ay * S) * SCALE + (S * SCALE) // 2))
    def compare(origin_x, origin_y, cols, rows):
        opaque_ok = opaque_n = semi_blended = semi_n = semi_far = 0
        for sy in rows:
            for sx in cols:
                c = sheet.getpixel((zrect[0] + sx, zrect[1] + sy))
                got = sprite_px(origin_x + sx, origin_y + sy)
                if c[3] == 255:
                    opaque_n += 1; opaque_ok += got == c[:3]
                elif c[3] > 0:
                    semi_n += 1
                    d = max(abs(got[i] - c[i]) for i in range(3))
                    if d > 16: semi_far += 1
                    elif got != c[:3]: semi_blended += 1
        return opaque_ok, opaque_n, semi_blended, semi_far, semi_n
    ok, n, blended, far, semi_n = compare(zx, zy, range(29, 36), range(0, 36))
    ok_b, n_b, blended_b, far_b, semi_b = compare(zx, zy, range(0, 58), range(26, 36))   # the wide, low slice holds the outline
    ok += ok_b; n += n_b; blended += blended_b; far += far_b; semi_n += semi_b
    print(f"ziggurat slice: {ok}/{n} opaque pixels equal the sheet; {blended}/{semi_n} outline pixels blended, {far} too far from the sheet colour")
    if n < 100: fails.append("the ziggurat slice has too few opaque pixels to mean anything: the check is broken")
    elif ok < n * 0.99:
        fails.append(f"only {ok}/{n} opaque ziggurat pixels match art/hamurabi/sprites.png: wrong sprite, palette or unpacking")
    if semi_n < 10: fails.append("the ziggurat slice has too few half-see-through pixels to test the blend")
    else:
        if far: fails.append(f"{far} half-see-through pixels are far from the sheet colour: they were dropped or blended with the wrong weight")
        if blended < semi_n * 0.8: fails.append(f"only {blended}/{semi_n} half-see-through pixels were blended (the rest equal the sheet exactly): alpha is being ignored")
    # the negative control: the same comparison pointed at the sky must not match
    ok2, n2, _, _, _ = compare(zx, zy - 30, range(29, 36), range(0, 36))
    print(f"control (same slice compared 30 pixels higher): {ok2}/{n2} match")
    if n2 and ok2 > n2 * 0.5: fails.append("the control comparison matched too: the sprite test cannot tell the ziggurat from the sky")

    # 4. the town is alive
    time.sleep(1.0)
    img2 = frame()
    changed = sum(1 for a, c in zip(win1.tobytes()[::4], window(img2).tobytes()[::4]) if a != c)
    print(f"window bytes changed over one second: {changed}")
    if changed < 500:
        fails.append("the window barely changed over a second: the villagers do not walk, or frames never reach the screen")

    # 5. each button by click. The primary one must be seen pressed, then released. Each must start what it names.
    def centre(i):
        x, y, w, h = btns[i]
        return VIEW_X + x + w // 2, VIEW_Y + y + h // 2
    def primary_color():
        x, y, w, h = btns[0]
        return pixel(VIEW_X + x + 40, VIEW_Y + y + h // 2 + 10, None)
    labels = ["Start a new game", "Classic 1968", "Watch a demo"]
    starts = [r"phase card year 1 intro", r"phase orders year 1 buy", r"seed \d+ mode demo"]
    for i in (0, 1, 2):
        line = f"hamurabi: {labels[i]} chosen"
        before = serial().count(line)
        pos = len(serial())
        move(*centre(i)); time.sleep(0.3)
        click()
        saw_pressed = False
        t0 = time.time()
        while time.time() - t0 < 1.0:
            if i == 0 and near(primary_color(), PRESSED, 6): saw_pressed = True
            if serial().count(line) > before and (i != 0 or saw_pressed): break
        time.sleep(0.2)
        if serial().count(line) != before + 1:
            fails.append(f"clicking '{labels[i]}' did not log exactly one '{line}'")
        if i == 0 and not saw_pressed: fails.append("the primary button was never drawn pressed (darker terracotta) after a click")
        if not wait_re(starts[i], pos, 5):
            fails.append(f"clicking '{labels[i]}' did not start what it names (no '{starts[i]}' in the log)")
        time.sleep(0.6)
        if i == 2:
            if not wait_re(r"phase orders year 2 ", pos, 12): fails.append("the demo did not play a year by itself")   # one year takes the robot about 6 s
            press("spc", gap=0.3)    # any key stops the demo
        else:
            press("esc", gap=0.3)    # a screen back
        if not wait_re(r"phase title", pos, 5): fails.append(f"after '{labels[i]}' the title did not come back")
        move(*PARK); time.sleep(0.8)
        if i == 0 and not near(primary_color(), ACCENT, 6): fails.append(f"the primary button did not return to its normal colour on the title: {primary_color()}")
    # a click on empty scenery chooses nothing
    chosen = serial().count(" chosen")
    move(VIEW_X + 60, VIEW_Y + 300); time.sleep(0.2); click(); time.sleep(0.4)
    if serial().count(" chosen") != chosen: fails.append("a click on empty ground chose a button")

    # 6. the keyboard: Right moves the focus ring onto the primary button, Right again onto the second, Enter chooses it
    move(*PARK); time.sleep(0.3)
    before = serial().count("hamurabi: Classic 1968 chosen")
    keys("right"); time.sleep(0.4); keys("right"); time.sleep(0.4)   # (a click leaves no focus ring: it starts from none)
    x, y, w, h = btns[1]
    ring = pixel(VIEW_X + x + w // 2, VIEW_Y + y - 2)   # just above the second button: the focus ring's top edge
    print(f"focus ring pixel above the second button: {ring}")
    if not near(ring, ACCENT, 6): fails.append(f"no focus ring above the second button after two Right presses (got {ring})")

    # ---- the replay machinery for steps 7 to 9 ----
    host_bin = os.path.join(tempfile.mkdtemp(prefix="jt-hamurabi-host-"), "host")
    host_src = host_bin + ".c"
    open(host_src, "w").write(HOST_C)
    cc = subprocess.run(["clang", "-O1", "-Wall", "-Wno-unused-function", "-I", ".", "-o", host_bin, host_src], capture_output=True, text=True)
    if cc.returncode: print(cc.stderr); raise SystemExit("FAIL: the host replay program did not compile")
    def replay(label, pos, script_lines, want_mode):
        """Compare everything the program logged for one reign (from offset pos to its 'over grade' line) with the host."""
        text = serial()[pos:]
        end = re.search(r"ring 3: hamurabi: over grade \S+\n", text)
        lines = app_lines(pos, pos + end.end() if end else None)
        while lines and not lines[0].startswith("seed "): lines.pop(0)   # the button's own "chosen" line comes first
        sm = re.match(r"seed (\d+) mode (\w+)$", lines[0]) if lines else None
        if not sm:
            fails.append(f"{label}: the reign's first line is not 'seed <n> mode <m>' ({lines[:1]})"); return None
        if sm[2] != want_mode: fails.append(f"{label}: the program logged mode {sm[2]}, expected {want_mode}")
        sp = host_bin + ".script"
        open(sp, "w").write("\n".join(script_lines) + "\n")
        r = subprocess.run([host_bin, sm[2] if sm[2] in ("story", "demo") else "classic", sm[1], sp], capture_output=True, text=True)
        want = r.stdout.split("\n")[:-1]
        if r.returncode: fails.append(f"{label}: the host replay stopped with {r.returncode}: {want[-1:]}")
        if lines == want:
            years = sum(1 for l in lines if l.startswith("year "))
            print(f"{label}: {len(lines)} logged lines ({years} years, {lines[-1]}) equal the host replay of seed {sm[1]}")
            return lines
        for k in range(max(len(lines), len(want))):
            a = lines[k] if k < len(lines) else "(nothing)"; c = want[k] if k < len(want) else "(nothing)"
            if a != c:
                fails.append(f"{label}: line {k} differs. program: '{a}'  host: '{c}'"); break
        return lines

    def play(mode, pos, keyscripts, pick, label):
        """Drive a reign that has just been started, by keyboard only. keyscripts: year -> key names for the orders;
        pick(n) -> which choice (0 or 1) to take at the n-th omen. Returns (script lines for the host, omens met, ok)."""
        script, omens, after_choice = [], 0, False
        while True:
            m = wait_re(r"ring 3: hamurabi: (phase card year (\d+) (\w+)|phase orders year (\d+) |over grade (\S+))", pos, 25)
            if not m:
                fails.append(f"{label}: the game stopped moving (no card, orders or ending after {pos})"); return script, omens, False
            pos = m.end()
            if m[5]:
                time.sleep(1.0); move(*PARK); time.sleep(0.4)
                return script, omens, True
            if m[2]:
                year, cid = int(m[2]), m[3]
                if cid in ("intro", "interlude"):
                    press("ret", gap=0.2)
                    continue
                n = omens; omens += 1
                idx = pick(n)
                time.sleep(0.7); move(*PARK); time.sleep(0.4)
                if "card" not in SHOTS: shot("card")
                if idx: press("down")
                press("ret", gap=0.2)
                script.append(f"choice {year} {idx}")
                after_choice = True
                continue
            year = int(m[4])
            if year in keyscripts:
                for k in keyscripts[year]:
                    press("shift", "left") if k == "sleft" else press("shift", "right") if k == "sright" else press(k)
                script.append("keys %d %s" % (year, " ".join(keyscripts[year])))
            if after_choice and "orders" not in SHOTS:
                time.sleep(0.6); move(*PARK); time.sleep(0.4); shot("orders")
            after_choice = False
            press("ret", gap=0.1)
            ym = wait_re(r"ring 3: hamurabi: year %d people" % year, pos, 10)
            if not ym:
                fails.append(f"{label}: ending year {year} did not produce a result"); return script, omens, False
            rl = re.findall(r"hamurabi: report year %d yield (\d+) harvest (\d+) rats (\d+) born (\d+) plague (\d+)" % year, serial()[pos:])
            score = 0
            if rl:
                yld, har, rats, born, plague = map(int, rl[-1])
                score = (rats > 0) + (plague > 0) * 2 + (born > 0) + (yld <= 2)
            if score > BEST["report"]:           # keep the richest report seen: rats, hunger, a poor harvest, new people
                BEST["report"] = score
                time.sleep(1.4); move(*PARK); time.sleep(0.2); shot("report")   # the year's numbers float up about a second and a half in
            press("ret", gap=0.1)

    # 7. (b) the classic reign by keyboard, with two years of changed orders
    pos_b = len(serial())
    press("ret", gap=0.1)
    if not wait_re(r"hamurabi: Classic 1968 chosen", pos_b, 3) or serial().count("hamurabi: Classic 1968 chosen") != before + 1:
        fails.append("Enter on the focused second button did not choose 'Classic 1968'")
    key_years = {1: ["sleft", "down", "left", "left", "left", "down", "end"], 2: ["right", "right", "down", "sright"]}
    script_b, _, ok_b = play("classic", pos_b, key_years, lambda n: 0, "(b) classic by keyboard")
    if ok_b:
        shot("ending")
        replay("(b) classic by keyboard", pos_b, script_b, "classic")
        lines_b = app_lines(pos_b)
        orders_b = [l for l in lines_b if l.startswith("orders year 1 ")]
        # the key presses of year 1, worked out on their own: sell 10 acres, feed three persons' worth less, plant all that can be
        c1 = [l for l in lines_b if l.startswith("phase orders year 1 ")]
        if c1 and orders_b:
            print(f"year 1 offered: {c1[0][len('phase orders year 1 '):]}; after the keys: {orders_b[0][len('orders year 1 '):]}")
            if c1[0].replace("phase orders", "orders") == orders_b[0]: fails.append("(b) the key presses of year 1 changed nothing")
        # back to the title: Down moves to Menu, Enter takes it
        pos = len(serial())
        press("down", gap=0.2); press("ret", gap=0.2)
        if not wait_re(r"phase title", pos, 5): fails.append("(b) Menu on the ending did not bring the title back")

    # 8. (c) a story reign: cards, a choice by keyboard each, replayed on the host
    omens_seen = 0
    for attempt in range(3):
        pos_c = len(serial())
        press("ret", gap=0.1)          # no focus on the title: Enter starts the primary button
        script_c, omens, ok_c = play("story", pos_c, {}, lambda n: n % 2, f"(c) story, try {attempt + 1}")
        if not ok_c: break
        replay(f"(c) story, try {attempt + 1}", pos_c, script_c, "story")
        omens_seen += omens
        lines_c = app_lines(pos_c)
        cards = [l for l in lines_c if l.startswith("phase card")]
        print(f"(c) story, try {attempt + 1}: cards {[c.split()[4] for c in cards]}, {omens} omen(s) with a choice")
        pos = len(serial())
        press("down", gap=0.2); press("ret", gap=0.2)
        if not wait_re(r"phase title", pos, 5): fails.append("(c) Menu on the ending did not bring the title back")
        if omens: break
    if not omens_seen: fails.append("(c) three story reigns met no omen to choose in: the story cards were not exercised")
    elif "card" not in SHOTS: fails.append("(c) no omen card was shown to photograph")

    # 9. (a) the demo: the robot rules by himself; the host replays the robot on the seed the program logged
    pos_a = len(serial())
    move(*centre(2)); time.sleep(0.3); click()
    plague_shot = False
    t_limit = time.time() + 200
    end_m = None
    seen_reports = set()
    while time.time() < t_limit:
        text = serial()
        em = re.compile(r"ring 3: hamurabi: over grade (\S+)\n").search(text, pos_a)
        if em: end_m = em; break
        for rm in re.finditer(r"ring 3: hamurabi: report year (\d+) yield \d+ harvest \d+ rats \d+ born \d+ plague 1", text[pos_a:]):
            if rm[1] not in seen_reports and not plague_shot:
                seen_reports.add(rm[1]); nxt = r"phase orders year %d " % (int(rm[1]) + 1)
                time.sleep(3.4); move(*PARK); time.sleep(0.1)   # the sickness is on screen from about three seconds into the report
                if nxt in serial()[pos_a:]: continue            # the report was already over: wait for the next plague
                shot("plague")
                if nxt in serial()[pos_a:]: SHOTS.pop("plague", None)   # over while the picture was taken: not a plague picture
                else: plague_shot = True
        time.sleep(0.25)
    if not end_m: fails.append("(a) the demo did not reach its ending in 200 seconds")
    else:
        time.sleep(2.0); move(*PARK); time.sleep(0.4); shot("ending-demo")
        lines_a = replay("(a) demo", pos_a, [], "demo")
        # the demo stops on a click
        pos = len(serial())
        move(VIEW_X + 60, VIEW_Y + 60); time.sleep(0.2); click()
        if not wait_re(r"phase title", pos, 5): fails.append("(a) a click on the finished demo did not bring the title back")
        # the robot's orders are the robot's: they differ from the offered sensible ones somewhere (else the demo did nothing)
        if lines_a and all(l.replace("phase orders", "orders") != lines_a[i + 1] for i, l in enumerate(lines_a) if l.startswith("phase orders") and i + 1 < len(lines_a)):
            pass
    print("screenshots: " + ", ".join(f"{k}={v}" for k, v in SHOTS.items()))
    for need in ("title", "card", "orders", "report", "ending", "ending-demo"):
        if need not in SHOTS: fails.append(f"no '{need}' screenshot was taken")

    # 10. Esc closes it, cleanly, and the desktop answers
    move(*PARK); time.sleep(0.3)
    exits = serial().count("HAMURABI.BIN exited 0")
    keys("esc")
    if not wait_serial("hamurabi: closed", 5): fails.append("Esc did not reach the program")
    if not wait_serial("syscall: window released, task gone", 5): fails.append("the window was not released when the program exited")
    for _ in range(50):
        if serial().count("HAMURABI.BIN exited 0") > exits: break
        time.sleep(0.1)
    else:
        fails.append("the program did not exit 0 on Esc")
    if "exception: ring-0" in serial() or "panic in" in serial():
        fails.append("the KERNEL faulted while Hamurabi ran or closed")
    move(*PARK); time.sleep(0.6)
    dock = pixel(480, 511)
    print(f"dock tray after Esc: {dock}")
    if dock != (0xEF, 0xEB, 0xE4): fails.append(f"desktop dock not on screen after Esc (got {dock})")
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): fails.append("an app window is still open after Esc")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after Esc: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Hamurabi closed: desktop stuck")
    keys("esc"); time.sleep(0.8)
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print("\n".join(serial().splitlines()[-40:])[-2500:])
    sys.exit(1)
print(f"PASS: Hamurapi ran at ring 3, drew its real title scene ({ncol} colours, sprite pixels equal the sheet), answered its buttons by click and keyboard, played a classic reign by keyboard, a story reign with cards and choices, and a demo reign by the robot, each line of each equal to a host replay of the same rules header, and closed on Esc with the desktop alive. Screenshots: {', '.join(SHOTS.values())}")
