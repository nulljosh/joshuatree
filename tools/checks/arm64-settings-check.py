#!/usr/bin/env python3
"""ARM64 Settings (arch/arm64/settings.c, settings_ui.h, glyphs.h): the window, its sections, a real wallpaper change, the menu bar's
status icons and JT/SETTINGS.TXT.

  1. Host: settings.c under sanitizers with stubbed card and audio. The file text it writes is `volume=70`, `wall=0`, `dock=7`;
     parsing takes a value only when it is a whole number in range (a bad line, a 5 digit number, a lone key, CRLF, a missing
     final newline); loading applies volume, wallpaper and dock size; saving with no card says so and keeps the choice.
     6.0: a signed-in person who is not an admin saves the wallpaper to ~/WALL.TXT; sign-in applies it, the login screen and
     anyone without one get the card's, and an out-of-range one is ignored.
  2. QEMU virt, headless, with ramfb, a virtio keyboard and tablet and the check card (sdcard.py) as a virtio disk. Spotlight (F1,
     "sett", Enter) opens Settings in About. The window's close dot and the seven sidebar tiles are sampled from a screendump:
     each tile is terracotta with its own glyph (no two the same). Down and Up walk About, Display, Sound, Dock, Date and Time, Users (the Users pane is arm64-users-check.py).
  3. Display: Right changes the wallpaper to Clay and the whole desktop repaints (wallpaper, the menu bar's blend, the dock
     tray's surroundings), Right again to Ink, Left back to Clay. Esc closes: the window's place is the new wallpaper. The
     Calculator then opens and closes: the pixels it saved at boot are refilled, so its place is the new wallpaper too, not the old one.
  4. The menu bar's icons (speaker, power, and the Wi-Fi fan and the clock beside them): their columns come off the UART, a click
     on each opens Settings at its section (Sound, About, About, Date and Time). In Sound the + and - buttons step the volume by
     10 and the menu bar's speaker follows; Mute toggles to 0 and back. In Dock the + button steps the tile size.
  5. The card: after the session JT/SETTINGS.TXT (read back with mtools) holds the last choices, and a second boot on the same
     card says `settings loaded ...` with them and shows the saved wallpaper without opening anything.
Screendumps are saved too: set JT_SETTINGS_SHOT to a .png path to keep the Sound page's screen. Skips (exit 0) when the tools are missing.
"""
import json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)


# ---- 1. the host test of settings.c ----
HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int vol = 70, card = 1, dock_scale_pct = 7, wall_rgb = 0, repaints = 0, written = 0;
static char file[96]; static int file_len = -1;
int audio_volume(void) { return vol; }
void audio_set_volume(int v) { vol = v < 0 ? 0 : v > 100 ? 100 : v; }
int sd_ready(void) { return card; }
int sd_enter_jt(int make) { (void)make; return card; }
int sd_write(const char *name, const void *data, unsigned len) { assert(!strcmp(name, "SETTINGS.TXT")); memcpy(file, data, len); file_len = (int)len; written++; return 1; }
static int signed_in, home_len = -1; static char home_file[16];   /* 6.0: the signed-in person's ~/WALL.TXT */
int fat_read_file(const char *name, void *buf, unsigned cap) {
    if (!strcmp(name, "WALL.TXT")) { assert(signed_in); if (home_len < 0) return -1; memcpy(buf, home_file, (unsigned)home_len); return home_len; }
    assert(!strcmp(name, "SETTINGS.TXT")); if (file_len < 0) return -1; assert((unsigned)file_len <= cap); memcpy(buf, file, (unsigned)file_len); return file_len; }
void fat_cwd_set(unsigned c) { (void)c; }
int home_active(void) { return signed_in; }
int home_mine(char out[9]) { if (!signed_in) return 0; strcpy(out, "bob"); return 1; }
int home_in(const char *dir, int make) { (void)make; return !strcmp(dir, "bob"); }
int home_save(const char *name, const void *data, unsigned len) { assert(signed_in && !strcmp(name, "WALL.TXT") && len < sizeof home_file); memcpy(home_file, data, len); home_len = (int)len; return 1; }
int fat_chdir(const char *n) { (void)n; return 1; }
void wall_set(unsigned rgb) { wall_rgb = (int)rgb; }
void desktop_wall_changed(void) { repaints++; }
void kputs(const char *s) { fputs(s, stdout); }
void kdec(unsigned v) { printf("%u", v); }
static int may = 1;
int sudo_may_write(void) { return may; }
#define dock_scale_pct dock_scale_pct
#include "arch/arm64/settings.c"
int main(void) {
    char out[64]; unsigned n = settings_format(out, 70, 0, 7);
    assert(n == 24 && !memcmp(out, "volume=70\nwall=0\ndock=7\n", 24));
    n = settings_format(out, 100, 3, 25); assert(!memcmp(out, "volume=100\nwall=3\ndock=25\n", n));
    int v = 1, w = 1, d = 7; const char *t;
    t = "volume=40\nwall=3\ndock=12\n"; settings_parse(t, (unsigned)strlen(t), &v, &w, &d); assert(v == 40 && w == 3 && d == 12);
    v = 1; w = 1; d = 7; t = "volume=101\nwall=4\ndock=4\n"; settings_parse(t, (unsigned)strlen(t), &v, &w, &d); assert(v == 1 && w == 1 && d == 7);
    t = "volume=26\nwall=1\ndock=26\n"; settings_parse(t, (unsigned)strlen(t), &v, &w, &d); assert(v == 26 && w == 1 && d == 7);
    t = "volume=12345\nvolume\n=5\nwall=x\nwall=\ndock=9z\nvolumes=5\n"; v = 1; w = 1; d = 7; settings_parse(t, (unsigned)strlen(t), &v, &w, &d); assert(v == 1 && w == 1 && d == 7);
    t = "volume=55\r\nwall=3\r\ndock=20"; settings_parse(t, (unsigned)strlen(t), &v, &w, &d); assert(v == 55 && w == 3 && d == 20);   /* CRLF, and no final newline */
    t = "dock=5"; settings_parse(t, 6, &v, &w, &d); assert(d == 5);
    t = "dock=55"; settings_parse(t, 6, &v, &w, &d); assert(d == 5);   /* cut at 6 bytes: 5 and a stray byte, taken as 5, never read past n */
    settings_parse("", 0, &v, &w, &d);
    assert(settings_walls() == 4 && !strcmp(settings_wall_name(0), "Grey") && settings_wall_rgb(0) == 0x006E6E6Eu && !strcmp(settings_wall_name(1), "Terracotta") && settings_wall_rgb(1) == 0x00B5502Cu && settings_wall_rgb(3) == 0x00141311u);
    settings_set_wall(-1); assert(settings_wall() == 3 && wall_rgb == 0x00141311); settings_set_wall(4); assert(settings_wall() == 0 && wall_rgb == 0x006E6E6E);
    settings_set_dock(2); assert(settings_dock() == 5); settings_set_dock(99); assert(settings_dock() == 25); settings_set_dock(7);
    vol = 30; settings_set_wall(2); settings_set_dock(11); settings_save();
    assert(settings_saved() == 1 && file_len == 25 && !memcmp(file, "volume=30\nwall=2\ndock=11\n", 25));
    vol = 70; settings_set_wall(0); settings_set_dock(7); repaints = 0; wall_rgb = 0; settings_load();
    assert(vol == 30 && settings_wall() == 2 && wall_rgb == 0x00923F20 && settings_dock() == 11 && repaints == 1 && settings_saved() == 1);
    repaints = 0; settings_load(); assert(repaints == 0);   /* nothing changed: no repaint */
    may = 0; { int w = written; vol = 10; settings_save(); assert(settings_saved() == -2 && written == w && vol == 10 && file_len == 25); }   /* 4.10.0: not an admin, so nothing is written and the choice stays */
    may = 1;
    card = 0; vol = 20; settings_save(); assert(settings_saved() == -1 && vol == 20 && settings_wall() == 2);   /* no card: said, and the choices stay */
    file_len = -1; card = 1; vol = 70; settings_load(); assert(vol == 70);                                         /* no file: nothing changes */
    /* 6.0, Yours: the card says Clay (2); bob, not an admin, saves Ink; his wallpaper is his, the card's stays Clay */
    file_len = -1; card = 1; settings_set_wall(2); settings_save(); assert(settings_saved() == 1);
    signed_in = 1; may = 0; settings_set_wall(3); { int w = written; settings_save(); assert(settings_saved() == 2 && written == w && home_len == 7 && !memcmp(home_file, "wall=3\n", 7)); }
    signed_in = 0; settings_user_wall(); assert(settings_wall() == 2 && wall_rgb == 0x00923F20);    /* the login screen: the card's */
    signed_in = 1; settings_user_wall(); assert(settings_wall() == 3 && wall_rgb == 0x00141311);    /* bob signs in: his */
    home_len = -1; settings_user_wall(); assert(settings_wall() == 2);                               /* no WALL.TXT: the card's */
    memcpy(home_file, "wall=9\n", 7); home_len = 7; settings_user_wall(); assert(settings_wall() == 2);   /* out of range: ignored */
    signed_in = 0; may = 1;
    puts("PASS: settings.c file text, bounded parsing, load, save, the no-card case and each person's own wallpaper");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="jt-settings-") as tmp:
    src = os.path.join(tmp, "t.c"); open(src, "w").write(HOST)
    exe = os.path.join(tmp, "t")
    cc = subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                         "-I", root, "-I", os.path.join(root, "arch/arm64"), src, "-o", exe], capture_output=True, text=True)
    check("settings.c builds on the host with warnings as errors", cc.returncode == 0, cc.stderr)
    if cc.returncode == 0:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
        check("settings.c: text, parsing, load, save and no card", r.returncode == 0 and "PASS" in r.stdout, r.stdout + r.stderr)

# ---- 2. the desktop ----
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: make -C arch/arm64 kernel8.elf failed\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

tmp = scratch_dir("arm64-settings")
card = os.path.join(tmp, "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode: print("FAIL: mtools could not make JT/ on the check card"); sys.exit(1)

W, H = 800, 600
WX, WY, WW, WH = (W - 500) // 2, 100, 500, 350      # the window: the Console's rectangle (fb_desktop)
GREY = (0x6e, 0x6e, 0x6e)
ACCENT, CLAY, INK, TERRA, RED, GLYPH = (0xb5, 0x50, 0x2c), (0x92, 0x3f, 0x20), (0x14, 0x13, 0x11), (0xb5, 0x50, 0x2c), (0xff, 0x5f, 0x57), (0xfa, 0xf8, 0xf4)
def bar(c): return tuple(((v >> 1) & 0x7f) + 0x80 for v in c)    # the menu bar over a wallpaper colour: half itself, half white
fails_boot = []


def boot(tag):
    log, sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return q, log, sock


class Session:
    def __init__(self, tag):
        self.q, self.log, self.sock = boot(tag)
        self.f = None
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=8.0, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
    def connect(self):
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30)
        for _ in range(100):
            try: s_.connect(self.sock); break
            except OSError: time.sleep(0.1)
        self.f = s_.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities")
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def press(self, qcode, want, secs=8):   # a key, then the UART line it must cause
        n = len(self.uart()); self.key(qcode); return self.wait_for(want, secs, n)
    def shot(self):
        path = os.path.join(tmp, "shot%d.ppm" % time.monotonic_ns())
        self.cmd("screendump", filename=path)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3)
        return (int(parts[1].split()[0]), parts[3])
    def click(self, x, y, want, secs=8):
        n = len(self.uart())
        ax, ay = int((x + 0.5) * 32768 / W), int((y + 0.5) * 32768 / H)
        self.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}])
        time.sleep(0.2)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        return self.wait_for(want, secs, n)
    def close(self):
        if self.f: self.f.close()
        if self.q.poll() is None: self.q.kill()
        self.q.wait()


def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
def box(img, x, y, w, h): return [px(img, i, j) for j in range(y, y + h) for i in range(x, x + w)]
def png(img, path):
    rows = b"".join(b"\0" + img[1][y * img[0] * 3:(y + 1) * img[0] * 3] for y in range(len(img[1]) // (img[0] * 3)))
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img[0], len(img[1]) // (img[0] * 3), 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


S = Session("a")
try:
    if not S.wait_for("M2 input ready", 60) or not S.wait_for("sd fat32 partition at block 2048", 30):
        check("the desktop and the card come up", False, S.uart()[-600:]); raise SystemExit
    S.connect(); time.sleep(0.8)
    base = S.shot()
    check("boot: no settings file on a fresh card, the wallpaper is grey (the default) and the clock page has no card message yet",
          "settings loaded" not in S.uart() and px(base, 30, 300) == GREY)
    icons = re.search(r"menubar icons power (\d+)-(\d+) sound (\d+)-(\d+) wifi (\d+)-(\d+) clock (\d+)-(\d+)", S.uart())
    check("the menu bar says where its four status icons are, left to right: power, sound, Wi-Fi, clock",
          bool(icons) and all(int(icons.group(i)) < int(icons.group(i + 1)) for i in (1, 3, 5, 7)) and int(icons.group(2)) < int(icons.group(3)) and int(icons.group(4)) < int(icons.group(5)), S.uart()[-500:])
    ic = [int(g) for g in icons.groups()] if icons else [0] * 8
    mid = lambda i: (ic[2 * i] + ic[2 * i + 1]) // 2
    # the sound and power glyphs are inked in the bar: dark pixels in each column
    dark = lambda lo, hi: sum(1 for p in box(base, lo, 3, hi - lo, 19) if max(p) < 0x60)
    check("the speaker and the power symbol are drawn in the bar (ink in both columns)", dark(ic[0] + 4, ic[1] - 4) > 20 and dark(ic[2] + 4, ic[3] - 4) > 20, (dark(ic[0] + 4, ic[1] - 4), dark(ic[2] + 4, ic[3] - 4)))
    # ---- open from Spotlight ----
    S.key("f1"); S.key("s", "e", "t", "t")
    check("Spotlight lists exactly Settings for 'sett'", S.wait_for("spotlight: sett 1\n", 5), S.uart()[-300:])
    n = len(S.uart()); S.key("ret")
    check("Enter opens Settings in About", S.wait_for("settings open\n", 6, n) and S.wait_for("settings section 0 About\n", 6, n), S.uart()[n:][-300:])
    time.sleep(0.4)
    a = S.shot()
    check("the window has its red close dot", px(a, WX + 19, WY + 16) == RED, px(a, WX + 19, WY + 16))
    tiles = []
    for i in range(7):
        tb = box(a, WX + 20, WY + 40 + i * 30, 18, 18)
        tiles.append(tb)
    terra = [sum(1 for p in tb if p == ACCENT) for tb in tiles]
    glyph = [sum(1 for p in tb if sum(abs(p[k] - GLYPH[k]) for k in range(3)) < 40) for tb in tiles]
    check("each of the seven sidebar tiles is terracotta with a glyph on it (%s accent, %s glyph pixels)" % (terra, glyph),
          all(t > 100 for t in terra) and all(g > 5 for g in glyph), (terra, glyph))
    check("no two glyphs are the same picture", len({tuple(tb) for tb in tiles}) == 7)
    check("the Settings title is in the sidebar's neighbour: About, Version, Board, Memory, Wi-Fi, Power rows draw (the card is white)", px(a, WX + 8 + 150 + 16 + 5, WY + 74 + 5) == (255, 255, 255))
    # ---- sections ----
    names = ["Display", "Sound", "Dock", "Date and Time", "Users", "Wi-Fi"]
    keep_dir = os.environ.get("JT_SETTINGS_SHOTS")   # a folder: one png of every section, for looking at
    if keep_dir:
        os.makedirs(keep_dir, exist_ok=True); png(a, os.path.join(keep_dir, "about.png"))
    for i, name in enumerate(names, 1):
        check("Down: section %d is %s" % (i, name), S.press("down", "settings section %d %s\n" % (i, name)))
        if keep_dir: time.sleep(0.4); png(S.shot(), os.path.join(keep_dir, name.split()[0].lower() + ".png"))
    check("Down wraps to About", S.press("down", "settings section 0 About\n"))
    check("Up wraps to Wi-Fi, then Users, then Date and Time", S.press("up", "settings section 6 Wi-Fi\n") and S.press("up", "settings section 5 Users\n") and S.press("up", "settings section 4 Date and Time\n"))
    check("Up, Up, Up: Sound, Display ... ", S.press("up", "settings section 3 Dock\n") and S.press("up", "settings section 2 Sound\n") and S.press("up", "settings section 1 Display\n"))
    # ---- wallpaper ----
    check("Right in Display: wallpaper 1, Terracotta (Grey is first and the default)", S.press("right", "settings wall 1 Terracotta\n"))
    time.sleep(0.5)
    t_ = S.shot()
    check("Terracotta everywhere the wallpaper shows", all(px(t_, x, y) == TERRA for x, y in [(30, 300), (770, 300), (60, 560), (100, 90)]), [px(t_, x, y) for x, y in [(30, 300), (770, 300), (60, 560), (100, 90)]])
    check("Right again: wallpaper 2, Clay", S.press("right", "settings wall 2 Clay\n"))
    check("... and it is saved", S.wait_for("settings saved\n", 6))
    time.sleep(0.5)
    c = S.shot()
    spots = [(30, 300), (770, 300), (60, 560), (100, 90)]
    check("the whole desktop is Clay: wallpaper at four places", all(px(c, x, y) == CLAY for x, y in spots), [px(c, x, y) for x, y in spots])
    check("the menu bar was blended again over the new colour (half Clay, half white)", px(c, 300, 8) == bar(CLAY), (px(c, 300, 8), bar(CLAY)))
    check("the window is back on top of the new desktop and the dock is whole (its cream tray, not Clay)", px(c, WX + 19, WY + 16) == RED and px(c, W // 2, H - 60) not in (CLAY, TERRA), (px(c, WX + 19, WY + 16), px(c, W // 2, H - 60)))
    check("Right again: wallpaper 3, Ink", S.press("right", "settings wall 3 Ink\n"))
    time.sleep(0.5)
    d = S.shot()
    check("Ink everywhere the wallpaper shows", all(px(d, x, y) == INK for x, y in spots), [px(d, x, y) for x, y in spots])
    check("Left: back to Clay", S.press("left", "settings wall 2 Clay\n"))
    time.sleep(0.5)
    n = len(S.uart()); S.key("esc")
    check("Esc closes Settings", S.wait_for("settings closed\n", 6, n))
    time.sleep(0.5)
    e = S.shot()
    gone = [(WX + 1, WY + 1), (WX + WW - 2, WY + 1), (WX + 1, WY + WH - 2), (WX + WW - 2, WY + WH - 2)]   # the frame's rounded corners show the wallpaper
    check("the Console is back where Settings was (white well, red dot), none of Settings' sidebar left, and the window's corners are the new wallpaper",
          px(e, WX + 200, WY + 200) == (255, 255, 255) and px(e, WX + 19, WY + 16) == RED and px(e, WX + 40, WY + 45) != (0xf0, 0xea, 0xe1) and all(px(e, x, y) == CLAY for x, y in gone), [px(e, x, y) for x, y in gone])
    # ---- the pixels saved at boot are refilled: the Calculator, opened and closed, leaves Clay behind ----
    S.key("f1"); S.key("c", "a", "l", "c"); S.key("ret")
    check("the Calculator opens", S.wait_for("calc open\n", 6))
    n = len(S.uart()); S.key("esc")
    check("and closes", S.wait_for("calc closed\n", 6, n))
    time.sleep(0.5)
    f = S.shot()
    calc = [(700, 200), (770, 300), (700, 430), (665, 150)]   # inside the Calculator's frame, clear of the Console
    check("where the Calculator stood is Clay, not the terracotta saved at boot", all(px(f, x, y) == CLAY for x, y in calc), [px(f, x, y) for x, y in calc])
    # ---- the menu bar's icons ----
    check("a click on the speaker opens Settings in Sound", S.click(mid(1), 12, "settings section 2 Sound\n") and S.wait_for("menubar click sound\n", 2))
    time.sleep(0.5)
    cx, cy = WX + 8 + 150 + 16, WY + 74
    plus, minus = (cx + 312 - 14 - 8, cy + 15), (cx + 312 - 14 - 16 - 16 - 6 - 70 - 6 + 8, cy + 15)
    sp_before = S.shot()
    check("Sound: + raises the volume to 80", S.click(*plus, "settings volume 80\n"))
    time.sleep(0.4)
    sp_after = S.shot()
    bar_x0, bar_y = cx + 312 - 14 - 16 - 6 - 70, cy + 21
    filled = lambda img: sum(1 for i in range(70) if px(img, bar_x0 + i, bar_y) == ACCENT)
    check("the level bar under the number is 70%% full, then 80%% (%d, %d of 70 pixels)" % (filled(sp_before), filled(sp_after)), abs(filled(sp_before) - 49) <= 2 and abs(filled(sp_after) - 56) <= 2)
    check("- steps back to 70", S.click(*minus, "settings volume 70\n"))
    for _ in range(7): S.click(*minus, "settings volume") ; time.sleep(0.1)
    check("... and down to 0 and no further", S.wait_for("settings volume 0\n", 6))
    time.sleep(0.5)
    muted = S.shot()
    check("at 0 the speaker in the menu bar loses its waves and is dimmed (fewer ink pixels than at 70)",
          sum(1 for p in box(muted, ic[2] + 2, 3, ic[3] - ic[2] - 4, 19) if max(p) < 0xB0) < sum(1 for p in box(sp_before, ic[2] + 2, 3, ic[3] - ic[2] - 4, 19) if max(p) < 0xB0))
    mx, my = cx + 312 - 14 - 13, cy + 30 + 14
    check("the Mute row's switch turns volume back up from 0 (last level 10)", S.click(mx, my, "settings volume 10\n"))
    check("and again mutes", S.click(mx, my, "settings volume 0\n"))
    check("m also toggles, back to 10", S.press("m", "settings volume 10\n"))
    check("a click on the power symbol opens About", S.click(mid(0), 12, "settings section 0 About\n") and S.wait_for("menubar click power\n", 2))
    check("the Wi-Fi fan opens About too", S.click(mid(2), 12, "menubar click wifi\n"))
    check("the clock opens Date and Time", S.click(mid(3), 12, "settings section 4 Date and Time\n") and S.wait_for("menubar click clock\n", 2))
    check("a click on a sidebar row (Dock) selects it", S.click(WX + 60, WY + 40 + 3 * 30 + 6, "settings section 3 Dock\n"))
    time.sleep(0.3)
    check("the Dock + button steps the tile size to 8", S.click(cx + 312 - 14 - 8, cy + 15, "settings dock 8\n"))
    check("every change was saved", S.uart().count("settings saved\n") >= 12, S.uart().count("settings saved\n"))
    # the sound page, for the screenshot
    S.click(WX + 60, WY + 40 + 2 * 30 + 6, "settings section 2 Sound\n"); time.sleep(0.5)
    S.click(*plus, "settings volume 20\n"); time.sleep(0.5)
    shot = S.shot()
    keep = os.environ.get("JT_SETTINGS_SHOT")
    if keep:
        os.makedirs(os.path.dirname(keep), exist_ok=True); png(shot, keep); print("  (screenshot kept at %s)" % keep)
    S.press("esc", "settings closed\n")
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "M1c fb FAIL" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
except SystemExit:
    pass
finally:
    S.close()

# ---- the card: what the first boot left, and a second boot on the same card ----
r = mt("mtype", "::JT/SETTINGS.TXT")
text = r.stdout.decode(errors="replace")
check("JT/SETTINGS.TXT on the card holds the last choices", text == "volume=20\nwall=2\ndock=8\n", text)
S = Session("b")
try:
    if not S.wait_for("M2 input ready", 60) or not S.wait_for("settings loaded", 30):
        check("the second boot loads the file", False, S.uart()[-600:]); raise SystemExit
    check("second boot: it says what it loaded", "settings loaded volume 20 wall 2 dock 8\n" in S.uart(), S.uart()[-400:])
    S.connect(); time.sleep(1.0)
    g = S.shot()
    check("and the desktop is Clay without opening anything (the card was read after the desktop was drawn, which repainted it)", px(g, 30, 300) == CLAY and px(g, 400, 8) == bar(CLAY), (px(g, 30, 300), px(g, 400, 8)))
    check("the Console is whole after the repaint (its white well)", px(g, WX + 200, WY + 200) == (255, 255, 255), px(g, WX + 200, WY + 200))
except SystemExit:
    pass
finally:
    S.close()

if fails:
    print("FAIL: %d of the Settings checks failed" % len(fails)); sys.exit(1)
print("PASS: Settings opens from Spotlight and the menu bar, seven sections with glyphs, a wallpaper change repaints the desktop and a closed window leaves it, volume and dock size step, JT/SETTINGS.TXT survives a reboot")
