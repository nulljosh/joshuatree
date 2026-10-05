#!/usr/bin/env python3
"""Hamurabi opens as a ring-3 window, draws its real title scene, answers its buttons and closes cleanly (2.7).

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
     must also be seen drawn pressed (the darker terracotta) and released again;
  6. moves the focus ring with the arrow keys and presses Enter: the second button is chosen, from the keyboard;
  7. presses Esc: the program must exit 0, release its window, and the desktop must take a click (Mail opens
     from the dock) afterwards.

There is no crash key on purpose: this app has none (the all-apps crash check is told so in its own file).

Discriminating: break the run-length unpacking (user/hamurabi.c unpack) or the palette and step 3 fails; stop calling
draw() or present and steps 2 and 4 fail; draw straight to the window while the compositor copies it and the
frames tear (the buttons vanish half the time); drop the alpha branch in blendpx and the outline test fails; make
a button's rectangle wrong and step 5 clicks nothing.

Usage: tools/checks/ring3hamurabi-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3hamurabi-serial.log"
DUMP = "/tmp/jt-ring3hamurabi.raw"
PNG = "/tmp/jt-hamurabi-title.png"
PNG_COPY = "/private/tmp/claude-501/-Users-joshua/eb3bf9f4-7225-4449-bd7a-ac7ecbec87fc/scratchpad/jt-title.png"
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
PARK = (480, 200)
ACCENT = (0xB5, 0x50, 0x2C)      # the primary button
PRESSED = (0x8C, 0x3E, 0x20)     # the same button while pressed

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=hamurabi",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False
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
    if "BUG" in log or "no heap" in log or "no window" in log:
        fails.append("the program or ring3app logged a BUG / no heap / no window line")

    # 2. a real picture
    img1 = frame()
    win1 = window(img1)
    os.makedirs(os.path.dirname(PNG_COPY), exist_ok=True)
    win1.save(PNG)
    try: shutil.copyfile(PNG, PNG_COPY)
    except OSError as e: print(f"(could not copy the screenshot to the scratchpad: {e})")
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

    # 5. each button by click. The primary one must be seen pressed, then released.
    def centre(i):
        x, y, w, h = btns[i]
        return VIEW_X + x + w // 2, VIEW_Y + y + h // 2
    def primary_color():
        x, y, w, h = btns[0]
        return pixel(VIEW_X + x + 40, VIEW_Y + y + h // 2 + 10, None)
    labels = ["Start a new game", "Classic 1968", "Watch a demo"]
    for i in (0, 1, 2):
        line = f"hamurabi: {labels[i]} chosen"
        before = serial().count(line)
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
        if i == 0:
            if not saw_pressed: fails.append("the primary button was never drawn pressed (darker terracotta) after a click")
            time.sleep(0.6)
            if not near(primary_color(), ACCENT, 6): fails.append(f"the primary button did not return to its normal colour after the press: {primary_color()}")
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
    keys("ret"); time.sleep(0.5)
    if serial().count("hamurabi: Classic 1968 chosen") != before + 1:
        fails.append("Enter on the focused second button did not choose 'Classic 1968'")

    # 7. Esc closes it, cleanly, and the desktop answers
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
    print("\n".join(l for l in serial().splitlines() if l not in ("present", "menubarredraw"))[-1800:])
    sys.exit(1)
print(f"PASS: Hamurabi ran at ring 3, drew its real title scene ({ncol} colours, sprite pixels equal the sheet), answered three buttons by click and one by keyboard, and closed on Esc with the desktop alive. Screenshot: {PNG}")
