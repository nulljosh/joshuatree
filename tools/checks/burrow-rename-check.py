#!/usr/bin/env python3
"""Burrow: the Files app was renamed and given a kit-fox icon.

What this proves, from the real sources (no QEMU, so it is fast):
  1. APPS[0] in kernel/kernel.c is "Burrow", and no APPS row is named
     "Files" any more. The dock label, title bar, Apps folder, phone grid,
     Launchpad and Spotlight all read that one field. The menu-bar list
     says Burrow too, and the ring-3 program's own title (user/burrow.c)
     and RING3_APPS row (kernel/ring3app.c) carry the name.
  2. Samantha's open_app matcher (match_app in user/samantha.c, compiled
     here for the host, its APPNAME table checked against the real APPS
     names) opens index 0 for
     "burrow", "files" and "file browser" (plus the "the ... app" forms),
     keeps every other app matching, and still refuses a name it does not
     know.
  3. Icon art slot 0 in kernel/icon_art.h is the new burrow art, and its
     bytes are not the old blue-folder art (sha256 pinned below).
  4. The authored SVG exists, the old files.svg is gone, and the generated
     header is current.

Fails on origin/main (APPS[0] is "Files", no alias table, slot 0 is
icon_art_files) and passes on this branch.
"""
import hashlib, os, re, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
OLD_FOLDER_SHA = "b214168dec7b34df23372dbaf059178e4db957896ca661b38e7337f288da5a6c"
fails = []


def check(ok, msg):
    print(("PASS: " if ok else "FAIL: ") + msg)
    if not ok:
        fails.append(msg)


def read(p):
    return open(os.path.join(ROOT, p)).read()


kernel = read("kernel/kernel.c")
m = re.search(r"const struct app APPS\[GUI_APP_COUNT\] = \{(.*?)\n\};", kernel, re.S)
names = re.findall(r'/\*\s*\d+\s*\*/\s*\{"([^"]+)"', m.group(1))
check(len(names) >= 25, "APPS table parsed (%d rows)" % len(names))
check(names[0] == "Burrow", "APPS[0] is %r, want 'Burrow'" % names[0])
check("Files" not in names, "no APPS row is named 'Files'")
menu = re.search(r'"About Joshua Tree",([^}]*?)"Shut Down"', kernel, re.S).group(1)
check('"Burrow"' in menu and '"Files"' not in menu, "menu-bar list says Burrow, not Files")
check(re.search(r'\{"Burrow",\s*user_burrow,\s*USER_BURROW_LEN,\s*"BURROW\.BIN"\}', read("kernel/ring3app.c")) is not None,
      "RING3_APPS has the Burrow row (BURROW.BIN)")
check(not os.path.exists(os.path.join(ROOT, "kernel/files.h")), "the in-kernel kernel/files.h is gone")
check("burrow: ring-3 window" in read("user/burrow.c"), "user/burrow.c identifies itself as burrow")

# --- match_app on the host, from user/samantha.c ---------------------------
sam = read("user/samantha.c")
a = sam.index("static const char *const APPNAME[]")
b = sam.index("/* Returns 1 with ar->reply filled when the tool is handled here")
sam_names = re.findall(r'"([^"]+)"', re.search(r"APPNAME\[\] = \{(.*?)\};", sam, re.S).group(1))
check(sam_names == names[:len(sam_names)], "samantha.c APPNAME matches the APPS names in order")
harness = r'''
#include <stdio.h>
#include <string.h>
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
%s
int main(void) {
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        printf("%%d\n", match_app(line));
    }
    return 0;
}
''' % sam[a:b]
with tempfile.TemporaryDirectory() as d:
    src = os.path.join(d, "h.c"); exe = os.path.join(d, "h")
    open(src, "w").write(harness)
    r = subprocess.run(["clang", "-w", "-o", exe, src], capture_output=True, text=True)
    check(r.returncode == 0, "match_app host harness compiles" + (": " + r.stderr[:300] if r.returncode else ""))
    if r.returncode == 0:
        cases = [("burrow", 0), ("Burrow", 0), ("the burrow app", 0), ("open burrow", 0),
                 ("files", 0), ("Files", 0), ("the files app", 0),
                 ("file browser", 0), ("the file browser", 0),
                 ("notes", names.index("Notes")), ("mail", names.index("Mail")),
                 ("chat", names.index("Samantha")), ("finder", -1), ("file", -1)]
        out = subprocess.run([exe], input="\n".join(c for c, _ in cases) + "\n",
                             capture_output=True, text=True).stdout.split()
        for (c, want), got in zip(cases, out):
            check(int(got) == want, "open_app %r -> %s (want %d)" % (c, got, want))

# --- icon art slot 0 -----------------------------------------------------
art = read("kernel/icon_art.h")
slot0 = re.search(r"ICON_ART\[\d+\] = \{\s*(\w+),", art).group(1)
check(slot0 == "icon_art_burrow", "ICON_ART[0] is %s, want icon_art_burrow" % slot0)
body = re.search(r"static const unsigned char %s\[\d+\] = \{(.*?)\};" % slot0, art, re.S)
data = bytes(int(x) for x in re.findall(r"\d+", body.group(1))) if body else b""
check(data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) > 8000, "slot 0 art is a real PNG (%d bytes)" % len(data))
check(hashlib.sha256(data).hexdigest() != OLD_FOLDER_SHA, "slot 0 art is no longer the old folder art")
check(os.path.exists(os.path.join(ROOT, "art/icons/burrow.svg")), "art/icons/burrow.svg exists")
check(not os.path.exists(os.path.join(ROOT, "art/icons/files.svg")), "old art/icons/files.svg is gone")
r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/gen/gen_icon_art.py"), "--check"], capture_output=True, text=True)
check(r.returncode == 0, "kernel/icon_art.h matches the SVGs")

print("FAIL: %d problem(s)" % len(fails) if fails else "PASS: Burrow rename, aliases and icon art all hold")
sys.exit(1 if fails else 0)
