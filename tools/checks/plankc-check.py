#!/usr/bin/env python3
"""plankc, the Plank compiler written in Plank, reads Plank exactly as plank.py does (the true bootstrap, docs/ROADMAP.md).

Builds user/plankc/plankc.pk with plank.py and runs it on every one of Plank's examples, on plankc's own files and on
user/plankc/cases.pk (a string over two lines, t.0.1, exponents, escapes, a comment inside brackets):
  lex (6.60.0, step 1): one token a line (line, kind, value) against plank.py's lex(). A string shows its escapes written
    back, or <interp> when it holds \\( ); a float as written.
  parse (6.61.0, step 2): the tree, one top-level item a line, against plank.py's Parser: each node its class, line and
    fields in order; [] a list, <> a tuple, {} a dict, _ nothing.
Discriminating: drop "??" from is_two in lex.pk and four examples differ; stop counting a string's newlines and cases.pk
drifts from its two-line string on; give "+" the precedence of "*" in parse.pk and every sum beside a product differs.
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
tmp = Path(scratch_dir("plankc"))
src = root / "user/plankc"
files = sorted((plank.parent / "examples").glob("*.pk")) + [src / n for n in ("lex.pk", "parse.pk", "gen.pk", "enc.pk", "rt.pk", "plankc.pk", "cases.pk", "tests/strings.pk", "tests/lists.pk", "tests/structs.pk", "tests/more.pk", "tests/dicts.pk", "tests/errors.pk", "tests/syscalls.pk", "tests/gc.pk", "tests/methods.pk", "tests/enums.pk", "tests/tuples.pk", "tests/closures.pk", "tests/floats.pk", "tests/builtins.pk")]

REF = r'''
import dataclasses, importlib.util, sys
spec = importlib.util.spec_from_file_location("plank", sys.argv[1]); pk = importlib.util.module_from_spec(spec); spec.loader.exec_module(pk)
def esc(s): return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\0", "\\0")
def show(t):
    if t.kind in ("nl", "eof"): return ""
    if t.kind == "int": return str(t.val)
    if t.kind == "float": return t.text
    if t.kind == "str": return "<interp>" if isinstance(t.val, list) else esc(t.val)
    return t.val
def d(v):
    if v is None: return "_"
    if isinstance(v, bool): return "true" if v else "false"
    if isinstance(v, int): return str(v)
    if isinstance(v, str): return '"' + esc(v).replace('"', '\\"') + '"'
    if isinstance(v, tuple): return "<" + " ".join(map(d, v)) + ">"
    if isinstance(v, list): return "[" + " ".join(map(d, v)) + "]"
    if isinstance(v, dict): return "{" + " ".join(d(k) + " " + d(x) for k, x in v.items()) + "}"
    fs = [(v.text or repr(v.val)) if isinstance(v, pk.Num) and f.name == "val" and v.ty == "float" else d(getattr(v, f.name))
          for f in dataclasses.fields(v) if f.name != "line"]
    return "(" + " ".join([type(v).__name__, str(v.line)] + fs) + ")"
for path in sys.argv[2:]:
    src = open(path).read()
    print("== lex " + path)
    for t in pk.lex(src): print("%d %s %s" % (t.line, t.kind, show(t)))
    print("== parse " + path)
    for item in pk.Parser(pk.lex(src)).program(): print(d(item))
'''
r = subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", "-c", REF, str(plank)] + [str(f) for f in files],
                   capture_output=True, text=True, timeout=300)
if r.returncode: print(r.stderr[-1500:]); print("FAIL: plank.py's lexer and parser did not run"); sys.exit(1)
want, cur = {}, None
for l in r.stdout.split("\n"):
    if l.startswith("== "): cur = tuple(l[3:].split(" ", 1)); want[cur] = []
    elif cur and l: want[cur].append(l)

exe = tmp / "plankc"
r = subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "build", str(src / "plankc.pk"), "-o", str(exe)],
                   capture_output=True, text=True, timeout=600)
if r.returncode: print((r.stdout + r.stderr)[-1500:]); print("FAIL: user/plankc/plankc.pk does not build"); sys.exit(1)

fails = []
for mode in ("lex", "parse"):
    for f in files:
        got = [l for l in subprocess.run([str(exe), mode, str(f)], capture_output=True, text=True, timeout=60).stdout.split("\n") if l]
        w = want[(mode, str(f))]
        if got != w:
            i = next((k for k in range(min(len(got), len(w))) if got[k] != w[k]), min(len(got), len(w)))
            print("  FAIL: %s %s, line %d:\n    want %s\n    got  %s" % (mode, f.name, i, (w[i] if i < len(w) else None, )[0], (got[i] if i < len(got) else None, )[0]))
            fails.append(mode + " " + f.name)
        else:
            print("  ok: %s %s (%d lines)" % (mode, f.name, len(w)))
# 6.93.0: plankc keeps only the functions main reaches. fib reaches printing and the heap, never JSON, regex or dates, so
# those runtime functions are gone from its assembly; plankc's own reaches its lexer and parser.
def globls(f):
    a = subprocess.run([str(exe), "asm", str(f), str(src / "rt.pk")], capture_output=True, text=True, timeout=120, cwd=str(f.parent)).stdout
    return {l[9:] for l in a.split("\n") if l.startswith("  .globl ")}
fib = globls(plank.parent / "examples" / "fib.pk")
for gone in ("pk_rt_json_quote", "pk_rt_rx_matches", "pk_rt_clock", "pk_rt_dict_items"):
    ok = gone not in fib
    print(("  ok: " if ok else "  FAIL: ") + "fib's assembly leaves out %s" % gone)
    if not ok: fails.append("shake " + gone)
for kept in ("main", "pk_main", "pk_fib", "pk_rt_print_str", "pk_rt_alloc"):
    ok = kept in fib
    print(("  ok: " if ok else "  FAIL: ") + "fib's assembly keeps %s" % kept)
    if not ok: fails.append("shake " + kept)
# 6.97.0: jtw_text's advances in rt.pk are libjt's, regenerated by tools/gen/plank-adv.py.
r = subprocess.run([sys.executable, str(root / "tools/gen/plank-adv.py"), "--check"], capture_output=True, text=True)
print(("  ok: " if r.returncode == 0 else "  FAIL: ") + r.stdout.strip())
if r.returncode: fails.append("plank-adv")
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: plankc, written in Plank, tokenizes and parses all %d files exactly as plank.py does, and keeps only what main reaches" % len(files))
