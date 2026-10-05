#!/usr/bin/env python3
"""Every app binary rides inside kernel.elf compressed (2.7.1). This proves the packing is lossless
and that the kernel's own inflate refuses damage, on the host, in seconds, with no QEMU.

For EVERY drivers/user_<app>.h rule in the Makefile it:
  1. runs tools/gen/gen_user_bin.py with the exact arguments the Makefile uses (output redirected
     to a scratch dir),
  2. compiles drivers/inflate.c natively with tools/inflate-host/main.c plus a generated table of
     all those headers,
  3. inflates each embedded array with inflate_checked() (the function ring3app_unpack() calls) and
     requires it to equal user/<app>.bin byte for byte, with USER_<APP>_LEN, _CLEN and _SUM right,
  4. then flips a byte (start, middle, end), truncates the stream, passes a wrong sum and a short
     buffer, and requires each one to be REJECTED. A harness that always said yes would pass step 3
     and fail here.
"""
import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(ROOT)
fails = []

def fail(msg):
    fails.append(msg); print("FAIL:", msg)

rules = re.findall(r'^\tpython3 tools/gen/gen_user_bin\.py (user/(\w+)\.bin) (drivers/(user_\w+)\.h) (user_\w+)$',
                   open("Makefile").read(), re.M)
if len(rules) < 25:
    sys.exit("FAIL: only %d gen_user_bin.py rules found in the Makefile, expected the whole set" % len(rules))

scratch = tempfile.mkdtemp(prefix="jt-usercompress-")
hdr_dir = os.path.join(scratch, "drivers")
os.makedirs(hdr_dir)
bin_dir = os.path.join(scratch, "bins")
os.makedirs(bin_dir)

apps = []
raw_total = comp_total = 0
for binp, app, hdrp, hdrname, sym in rules:
    if not os.path.exists(binp):
        fail("%s is not built (run make kernel.elf first)" % binp); continue
    out = os.path.join(hdr_dir, hdrname + ".h")
    r = subprocess.run(["python3", "tools/gen/gen_user_bin.py", binp, out, sym], capture_output=True, text=True)
    if r.returncode:
        fail("gen_user_bin.py %s: %s" % (binp, r.stderr.strip())); continue
    text = open(out).read()
    def macro(n):
        m = re.search(r'#define %s_%s (0x[0-9a-f]+|\d+)u' % (sym.upper(), n), text)
        return int(m.group(1), 0) if m else None
    ln, cl, su = macro("LEN"), macro("CLEN"), macro("SUM")
    if None in (ln, cl, su):
        fail("%s lacks _LEN/_CLEN/_SUM" % hdrname); continue
    if ln != os.path.getsize(binp):
        fail("%s: _LEN %d != size of %s" % (hdrname, ln, binp))
    n_bytes = len(re.findall(r'0x[0-9a-f]{2},', text[text.index("[] = {"):text.index("};")]))
    if n_bytes != cl:
        fail("%s: array holds %d bytes but _CLEN says %d" % (hdrname, n_bytes, cl))
    raw_total += ln; comp_total += cl
    apps.append((app, sym))
    link = os.path.join(bin_dir, app + ".bin")
    os.symlink(os.path.abspath(binp), link)

table = ['#include "%s.h"' % h for _, _, _, h, _ in rules]
table.append("struct app { const char *name; const unsigned char *packed; unsigned clen, len, sum; };")
table.append("const struct app APPS[] = {")
for app, sym in apps:
    u = sym.upper()
    table.append('  {"%s", %s, %s_CLEN, %s_LEN, %s_SUM},' % (app, sym, u, u, u))
table.append("};")
table.append("const unsigned NAPPS = sizeof APPS / sizeof APPS[0];")
open(os.path.join(scratch, "table.c"), "w").write("\n".join(table) + "\n")

exe = os.path.join(scratch, "harness")
cc = subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Wno-unused-const-variable", "-I" + hdr_dir, "-Idrivers", "-Itools/png-host",
                     "-o", exe, "tools/inflate-host/main.c", os.path.join(scratch, "table.c"), "drivers/inflate.c"],
                    capture_output=True, text=True)
if cc.returncode:
    print(cc.stderr); sys.exit("FAIL: harness did not compile")

run = subprocess.run([exe, bin_dir], capture_output=True, text=True)
lines = run.stdout.splitlines()
oks = sum(1 for l in lines if l.startswith("ok "))
rej = sum(1 for l in lines if l.startswith("rejected "))
for l in lines:
    if l.startswith("FAIL") or l.endswith("failures"):
        print(l)
if run.returncode:
    fail("harness exited %d" % run.returncode)
if oks != len(apps):
    fail("%d of %d apps round-tripped" % (oks, len(apps)))
if rej != 6 * len(apps):
    fail("%d rejections seen, expected %d (6 per app)" % (rej, 6 * len(apps)))
print("%d apps: %d bytes raw -> %d bytes embedded (%.1f%%); %d round trips byte for byte, %d damaged copies rejected"
      % (len(apps), raw_total, comp_total, 100.0 * comp_total / raw_total, oks, rej))
for f in os.listdir(bin_dir): os.unlink(os.path.join(bin_dir, f))
if fails:
    sys.exit("FAIL: %d problem(s)" % len(fails))
print("PASS: every embedded app inflates to exactly its binary, and damage is refused")
