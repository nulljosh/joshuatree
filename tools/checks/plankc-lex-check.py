#!/usr/bin/env python3
"""plankc's tokenizer matches Plank's own (6.60.0, step 1 of the true bootstrap in docs/ROADMAP.md).

Builds user/plankc/lex.pk, a tokenizer written in Plank, with plank.py, runs it on every one of Plank's examples, on
itself and on user/plankc/cases.pk (a string over two lines, t.0.1, exponents, escapes, a comment inside brackets), and
holds what it prints (line, kind, value, one token a line) against plank.py's lex() on the same file, line for line. A string's value is shown with its escapes written back, or <interp> when it holds \\( ); a float as written.
Discriminating: drop "??" from is_two in lex.pk and optionals.pk differs; stop counting a string's newlines and cases.pk drifts
from its two-line string on.
Skips (exit 0) without uv or Plank (PLANK=path/to/plank.py, default ~/Documents/Code/plank/plank.py; CI pins it).
"""
import os, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = Path(__file__).resolve().parents[2]
plank = Path(os.environ.get("PLANK", Path.home() / "Documents/Code/plank/plank.py"))
if not shutil.which("uv") or not plank.exists():
    print("SKIP: uv or Plank (%s) missing" % plank); sys.exit(0)
tmp = Path(scratch_dir("plankc-lex"))
lexer = root / "user/plankc/lex.pk"
files = sorted((plank.parent / "examples").glob("*.pk")) + [lexer, root / "user/plankc/cases.pk"]

REF = r'''
import importlib.util, sys
spec = importlib.util.spec_from_file_location("plank", sys.argv[1]); pk = importlib.util.module_from_spec(spec); spec.loader.exec_module(pk)
def show(t):
    if t.kind in ("nl", "eof"): return ""
    if t.kind == "int": return str(t.val)
    if t.kind == "float": return t.text
    if t.kind == "str":
        if isinstance(t.val, list): return "<interp>"
        return t.val.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\0", "\\0")
    return t.val
for path in sys.argv[2:]:
    print("== " + path)
    for t in pk.lex(open(path).read()): print("%d %s %s" % (t.line, t.kind, show(t)))
'''
r = subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", "-c", REF, str(plank)] + [str(f) for f in files],
                   capture_output=True, text=True, timeout=300)
if r.returncode: print(r.stderr[-1500:]); print("FAIL: plank.py's lexer did not run"); sys.exit(1)
want, cur = {}, None
for l in r.stdout.split("\n"):
    if l.startswith("== "): cur = l[3:]; want[cur] = []
    elif cur and l: want[cur].append(l)

exe = tmp / "lex"
r = subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "build", str(lexer), "-o", str(exe)],
                   capture_output=True, text=True, timeout=600)
if r.returncode: print((r.stdout + r.stderr)[-1500:]); print("FAIL: user/plankc/lex.pk does not build"); sys.exit(1)

fails = []
for f in files:
    got = [l for l in subprocess.run([str(exe), str(f)], capture_output=True, text=True, timeout=60).stdout.split("\n") if l]
    w = want[str(f)]
    if got != w:
        i = next((k for k in range(min(len(got), len(w))) if got[k] != w[k]), min(len(got), len(w)))
        print("  FAIL: %s, token %d: want %r, got %r" % (f.name, i, w[i] if i < len(w) else None, got[i] if i < len(got) else None))
        fails.append(f.name)
    else:
        print("  ok: %s (%d tokens)" % (f.name, len(w)))
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: plankc's tokenizer, written in Plank, reads all %d files exactly as plank.py does" % len(files))
