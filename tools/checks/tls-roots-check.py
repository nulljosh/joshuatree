#!/usr/bin/env python3
"""The whole web's roots (6.11.0): the Pi's trust anchors hold every root in Mozilla's list, each once.

Host only. Runs tools/gen/tls-ta.py over arch/arm64/certs/*.pem the way the arm64 Makefile does and checks:
  - every certificate in certs/mozilla-roots.pem (curl's extract of Mozilla's list) is an anchor;
  - a root that is in both the bundle and an older single file is kept once (TAS_NUM is the unique count);
  - ISRG Root X1 (Let's Encrypt) is there, as the most common root on the web;
  - the bundle is under 18 months old. Refresh it with
      curl -fsSL https://curl.se/ca/cacert.pem -o arch/arm64/certs/mozilla-roots.pem
That a fetch through those anchors verifies, and that a root outside them is refused, is arm64-tls-check.py.

Discriminating: drop the dedupe in tls-ta.py and TAS_NUM counts the shared roots twice.
Usage: tools/checks/tls-roots-check.py   (from the repo root)
"""
import datetime, glob, importlib.util, os, re, subprocess, sys, tempfile

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
gen = os.path.join(root, "tools/gen/tls-ta.py")
spec = importlib.util.spec_from_file_location("tls_ta", gen); ta = importlib.util.module_from_spec(spec); spec.loader.exec_module(ta)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (" (%s)" % detail if detail and not ok else ""))
    if not ok: fails.append(name)

bundle = os.path.join(root, "arch/arm64/certs/mozilla-roots.pem")
pems = sorted(glob.glob(os.path.join(root, "arch/arm64/certs/*.pem")))
every = [ta.anchor(der) for p in pems for der in ta.pems(p)]
moz = [ta.anchor(der) for der in ta.pems(bundle)]
with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, "tls_ta.h")
    subprocess.run([sys.executable, gen, out] + pems, check=True, capture_output=True)
    h = open(out).read()
num = int(re.search(r"#define TAS_NUM (\d+)", h).group(1))
check("Mozilla's list is there (%d roots)" % len(moz), len(moz) >= 100)
check("every root is an anchor, each once (%d unique of %d)" % (len(set(every)), len(every)), num == len(set(every)), "TAS_NUM %d" % num)
check("roots in both the bundle and a single file were folded", len(set(every)) < len(every))
check("ISRG Root X1 is trusted", any(b"ISRG Root X1" in dn for dn, _ in moz))
m = re.search(r"Certificate data from Mozilla as of: \w+ (\w+ +\d+) [\d:]+ (\d{4})", open(bundle).read())
age = (datetime.date.today() - datetime.datetime.strptime(" ".join(m.group(1).split()) + " " + m.group(2), "%b %d %Y").date()).days if m else 10**6
check("the bundle is under 18 months old (%d days)" % age, age < 550)
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the Pi trusts every root in Mozilla's list (%d anchors, shared roots kept once, ISRG Root X1 among them), from a bundle %d days old" % (num, age))
