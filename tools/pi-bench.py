#!/usr/bin/env python3
"""Run a Terminal command on a Pi that said `bench` (13.16.0), from this Mac.

The Pi asks the relay (tools/claude-relay/relay.py --bench-dir FOLDER) for its next command every two seconds. This
tool puts a line in FOLDER/queue, waits for the Pi to take it and send back what it printed, and prints that.

  tools/pi-bench.py gpu                 one command, its output
  tools/pi-bench.py "print NOTE.TXT"    quote a line with spaces
  tools/pi-bench.py --status            when the Pi last asked
  --dir FOLDER   the relay's --bench-dir (default ~/.cache/joshuatree/bench)
  --wait SECS    how long to wait for the output (default 60)
Exit 0 with the output, 1 when the Pi never took the line or never answered.
"""
import argparse, os, sys, time

ap = argparse.ArgumentParser(add_help=True)
ap.add_argument("line", nargs="*")
ap.add_argument("--dir", default="~/.cache/joshuatree/bench")
ap.add_argument("--wait", type=float, default=60)
ap.add_argument("--status", action="store_true")
a = ap.parse_args()
d = os.path.expanduser(a.dir)
os.makedirs(d, mode=0o700, exist_ok=True)
log, queue, seen = (os.path.join(d, n) for n in ("log", "queue", "seen"))

def last_seen():
    try: return time.time() - float(open(seen).read())
    except (OSError, ValueError): return None

if a.status or not a.line:
    s = last_seen()
    print("the Pi has never asked" if s is None else "the Pi last asked %.0f s ago%s" % (s, "" if s < 90 else " (bench is off, or it is busy)"))
    sys.exit(0 if s is not None and s < 90 else 1)

line = " ".join(a.line)
if len(line) > 200 or any(not " " <= c <= "~" for c in line): sys.exit("pi-bench: one line of plain ASCII, 200 characters at most")
start = os.path.getsize(log) if os.path.exists(log) else 0
with open(queue, "a", encoding="utf-8") as fh: fh.write(line + "\n")
mark = ("\n$ " + line + "\n").encode()
end, taken, out, quiet = time.time() + a.wait, False, b"", 0.0
while time.time() < end:
    time.sleep(0.25)
    data = open(log, "rb").read()[start:] if os.path.exists(log) else b""
    at = data.find(mark)
    if at < 0: continue
    taken = True
    got = data[at + len(mark):]
    nxt = got.find(b"\n$ ")                      # the next command's marker ends this one's output
    if nxt >= 0: got = got[:nxt]
    if got and got == out:
        quiet += 0.25
        if quiet >= 0.75: break                   # the output came and has stopped growing
    else: out, quiet = got, 0.0
if not taken:
    s = last_seen()
    sys.exit("pi-bench: the Pi did not take the line (%s). Is `bench` on?" % ("it has never asked" if s is None else "it last asked %.0f s ago" % s))
if not out: sys.exit("pi-bench: the Pi took the line but sent nothing back in %.0f s" % a.wait)
sys.stdout.write(out.decode("latin-1"))
