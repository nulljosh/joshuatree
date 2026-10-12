#!/usr/bin/env python3
"""The saved Bluetooth pairing (arch/arm64/btpair.h), built on the host with clang and exercised directly.

btpair.h is the record JT/BTPAIR.BIN holds: magic, version, address type, address, LTK, Rand, EDIV, name, a sum. It has no
kernel calls, so a small C program includes it. The check proves:
  1. round trip: pack then parse gives back every field; a long name is cut to 29 characters and stays NUL terminated.
  2. rejects: bad magic, a wrong version, every length short of a whole record, a flipped byte anywhere (the sum), an all
     zero key and an address type above 1, each with its own plain error word; the output record is left untouched.
  3. no key in any printed string: the error words and the program's whole output never hold the fake key, in either
     byte order, as raw bytes or hex; btpair.h has no print call; and bt.c never hands the key arrays to a print helper.
  4. bt.c is wired to it: it includes the header, saves through sd_write, has `bt forget` and the saved pairing line.
The fake key is an obvious made-up pattern, never a real one. The reconnect itself needs the real board (QEMU has no radio).
Skips (exit 0) when clang is missing.
Usage: tools/checks/bt-pair-check.py   (from the repo root)
"""
import os, re, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
hdr = os.path.join(root, "arch/arm64/btpair.h")
if not shutil.which("clang"):
    print("SKIP: clang not installed"); sys.exit(0)

FAKE_KEY = bytes([0xF0 + (i % 4) * 3 + (i // 4) for i in range(16)])   # f0 f3 f6 f9 f1 ... : obviously not random
FAKE_RAND = bytes(range(0xE0, 0xE8))

C = r'''
#include <stdio.h>
#include <string.h>
#include "btpair.h"
static void hexdump(const char *tag, const unsigned char *b, unsigned n) { printf("%s ", tag); for (unsigned i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
int main(void) {
    struct btp r, back;
    memset(&r, 0, sizeof r);
    r.type = 1; r.ediv = 0xBEEF;
    unsigned char a[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    unsigned char k[16] = { %KEY% }, ra[8] = { %RAND% };
    memcpy(r.addr, a, 6); memcpy(r.ltk, k, 16); memcpy(r.rand, ra, 8);
    strcpy(r.name, "Fake Mouse 3000");
    unsigned char rec[BTP_SIZE];
    printf("pack %d\n", btp_pack(rec, &r));
    hexdump("rec", rec, BTP_SIZE);                    /* the raw record is the one place the key is allowed to appear */
    memset(&back, 0, sizeof back);
    printf("parse %d\n", btp_parse(rec, BTP_SIZE, &back));
    printf("same %d\n", back.type == 1 && back.ediv == 0xBEEF && !memcmp(back.addr, a, 6) && !memcmp(back.ltk, k, 16)
                        && !memcmp(back.rand, ra, 8) && !strcmp(back.name, "Fake Mouse 3000"));
    printf("size %d\n", BTP_SIZE);
    /* a long name is cut, still NUL terminated */
    struct btp lg = r; memset(lg.name, 'n', BTP_NAME);
    unsigned char rl[BTP_SIZE]; struct btp bl;
    printf("longpack %d\n", btp_pack(rl, &lg));
    printf("longparse %d\n", btp_parse(rl, BTP_SIZE, &bl));
    printf("longname %d %d\n", (int)strlen(bl.name), rl[BTP_SIZE - 3]);
    /* rejects */
    unsigned char m[BTP_SIZE];
    struct btp keep; memset(&keep, 0x7E, sizeof keep); struct btp t = keep;
    memcpy(m, rec, BTP_SIZE); m[0] = 'X';
    printf("badmagic %d %s\n", btp_parse(m, BTP_SIZE, &t), btp_why(btp_parse(m, BTP_SIZE, &t)));
    memcpy(m, rec, BTP_SIZE); m[4] = 2;
    printf("badver %d %s\n", btp_parse(m, BTP_SIZE, &t), btp_why(btp_parse(m, BTP_SIZE, &t)));
    int shortok = 1;
    for (unsigned n = 0; n < BTP_SIZE; n++) if (btp_parse(rec, n, &t) != BTP_SHORT) shortok = 0;
    printf("short %d %s\n", shortok, btp_why(BTP_SHORT));
    int flipok = 1;                                   /* any single flipped byte is caught: by the magic, the version or the sum */
    for (unsigned i = 0; i < BTP_SIZE; i++) { memcpy(m, rec, BTP_SIZE); m[i] ^= 0x40; if (btp_parse(m, BTP_SIZE, &t) == BTP_OK) flipok = 0; }
    printf("flip %d %s\n", flipok, btp_why(BTP_SUM));
    struct btp z = r; memset(z.ltk, 0, 16);
    printf("zerokey %d %s\n", btp_pack(m, &z), btp_why(BTP_BAD));
    struct btp ty = r; ty.type = 2;
    printf("badtype %d\n", btp_pack(m, &ty));
    /* a whole record whose key is zero, with a sum that is right, is still refused */
    memcpy(m, rec, BTP_SIZE); memset(m + 12, 0, 16);
    unsigned short s = btp_sum(m, BTP_SIZE - 2); m[BTP_SIZE - 2] = (unsigned char)s; m[BTP_SIZE - 1] = (unsigned char)(s >> 8);
    printf("zeroparse %d\n", btp_parse(m, BTP_SIZE, &t));
    printf("untouched %d\n", !memcmp(&t, &keep, sizeof t));
    printf("why %s|%s|%s|%s|%s|%s\n", btp_why(BTP_OK), btp_why(BTP_SHORT), btp_why(BTP_MAGIC), btp_why(BTP_VER), btp_why(BTP_SUM), btp_why(BTP_BAD));
    return 0;
}
'''.replace("%KEY%", ", ".join("0x%02x" % b for b in FAKE_KEY)).replace("%RAND%", ", ".join("0x%02x" % b for b in FAKE_RAND))

fails = []
def check(ok, what):
    if not ok: fails.append(what)

tmp = scratch_dir("bt-pair")
src, exe = os.path.join(tmp, "t.c"), os.path.join(tmp, "t")
open(src, "w").write(C)
cc = subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", os.path.dirname(hdr), src, "-o", exe],
                    capture_output=True, text=True)
if cc.returncode:
    print("FAIL: btpair.h does not build on the host\n" + cc.stderr); sys.exit(1)
run = subprocess.run([exe], capture_output=True, text=True, timeout=60)
out = run.stdout
if run.returncode: print("FAIL: the test program exited", run.returncode, run.stderr); sys.exit(1)
v = {}
for ln in out.splitlines():
    w = ln.split(" ", 1)
    v[w[0]] = w[1] if len(w) > 1 else ""

check(v.get("pack") == "0", "pack of a good record")
check(v.get("parse") == "0", "parse of a good record")
check(v.get("same") == "1", "round trip keeps every field")
check(v.get("size") == "70", "record is 70 bytes")
check(v.get("longpack") == "0" and v.get("longparse") == "0", "long name packs and parses")
check(v.get("longname") == "29 0", "long name is cut to 29 characters and NUL terminated")
check(v.get("badmagic", "").startswith("-2 not a pairing file"), "bad magic is refused")
check(v.get("badver", "").startswith("-3 a different version"), "wrong version is refused")
check(v.get("short", "").startswith("1 "), "every truncation is refused")
check(v.get("flip", "").startswith("1 "), "a flipped byte anywhere is refused")
check(v.get("zerokey", "").startswith("-5 unusable"), "an all zero key is not packed")
check(v.get("badtype") == "-5", "address type above 1 is not packed")
check(v.get("zeroparse") == "-5", "a whole record with a zero key is refused on parse")
check(v.get("untouched") == "1", "a refused parse leaves the output untouched")
check(v.get("why") == "ok|too short|not a pairing file|a different version|damaged|unusable", "error words")

# the key shows only in the raw record line; nothing else the program printed holds it, in either order, raw or hex
rest = "\n".join(l for l in out.splitlines() if not l.startswith("rec "))
for name, b in (("key", FAKE_KEY), ("rand", FAKE_RAND)):
    for order, bb in (("forward", b), ("reversed", b[::-1])):
        check(bb.hex() not in rest.lower(), "%s (%s, hex) appears outside the record dump" % (name, order))
        check(bb not in rest.encode("latin-1", "replace"), "%s (%s, raw) appears outside the record dump" % (name, order))
rec_hex = [l.split()[1] for l in out.splitlines() if l.startswith("rec ")][0]
check(FAKE_KEY.hex() in rec_hex and len(rec_hex) == 140, "the record holds the key, 70 bytes")

h = open(hdr).read()
check(not re.search(r"\b(printf|puts|kputs|put_hex|put|say|cmd_out)\s*\(", re.sub(r"/\*.*?\*/", "", h, flags=re.S)), "btpair.h has a print call")
check("#include" not in h, "btpair.h has an include (it must stand alone)")

b = open(os.path.join(root, "arch/arm64/bt.c")).read()
check('#include "btpair.h"' in b, "bt.c includes btpair.h")
check(re.search(r'sd_write\(PAIR_FILE', b) is not None, "bt.c saves with sd_write")
check('"BTPAIR.BIN"' in b, "bt.c names JT/BTPAIR.BIN")
check('"bt: pairing saved"' in b, "bt.c logs pairing saved")
check('bt: saved pairing: ' in b and '"yes"' in b and '"no"' in b, "bt shows saved pairing yes or no")
check(re.search(r'has\(q \+ i, "forget"', b) is not None, "bt forget is parsed")
# no print helper ever gets the key arrays or the record
bad = re.findall(r"(?:put|put_hex|put_dec|kputs|cmd_out|say)\s*\([^;]*\b(?:ltk|krand|kediv|stk|r\.ltk|r\.rand|r\.ediv)\b", b)
check(not bad, "bt.c hands a key to a print helper: %r" % bad)

if fails:
    print("FAIL: bt-pair-check")
    for f in fails: print("  -", f)
    sys.exit(1)
print("OK: bt-pair-check, record round trip, rejects and no key in any printed string (the reconnect itself needs the Pi)")
