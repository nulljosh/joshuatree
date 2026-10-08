#!/usr/bin/env python3
"""Samantha's Pi actions: [[note TEXT]] and [[led blink]] at the end of an answer.

Host-only, no hardware. Compiles the real take_actions block out of arch/arm64/ask.c with clang and feeds it replies:
known markers are stripped and recorded, everything else stays in the printed text. Also checks the relay's system
prompt names both actions, so the model knows they exist.
"""
import os, re, subprocess, sys, tempfile

src = open("arch/arm64/ask.c").read()
m = re.search(r"/\* pi-actions begin.*?/\* pi-actions end \*/", src, re.S)
if not m:
    print("FAIL: no pi-actions block in arch/arm64/ask.c"); sys.exit(1)
relay = open("tools/claude-relay/relay.py").read()
prompt = re.search(r"SAMANTHA = \((.*?)\)\n", relay, re.S)
if not prompt or "[[note TEXT]]" not in prompt.group(1) or "[[led blink]]" not in prompt.group(1):
    print("FAIL: the relay's SAMANTHA prompt does not name [[note TEXT]] and [[led blink]]"); sys.exit(1)

LONG = "[[note " + "x" * 80 + "]]"
CASES = [   # reply -> printed text, actions as "kind:text"
    ("Done.\n[[note hello there]]", "Done.", ["1:hello there"]),
    ("Blinking.\n[[led blink]]\n", "Blinking.", ["2:"]),
    ("Hi.\n[[reboot]]", "Hi.\n[[reboot]]", []),
    ("Hi.\n" + LONG, "Hi.\n" + LONG, []),
    ("Say [[led blink]] to blink.", "Say [[led blink]] to blink.", []),
    ("Ok.\n[[note a]]\r\n[[led blink]]  ", "Ok.", ["1:a", "2:"]),
    ("Plain answer\nover two lines", "Plain answer\nover two lines", []),
    ("[[note ]]", "[[note ]]", []),
]
harness = m.group(0) + r"""
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    for (int c = 1; c < argc; c++) {
        static char buf[8192]; unsigned n = (unsigned)strlen(argv[c]);
        memcpy(buf, argv[c], n);
        unsigned k = take_actions(buf, n);
        fwrite(buf, 1, k, stdout); putchar('\x01');
        for (unsigned a = 0; a < act_n; a++) printf("%d:%s\x02", act_kind[a], act_text[a]);
        putchar('\x03');
    }
    return 0;
}
"""
with tempfile.TemporaryDirectory() as d:
    c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
    open(c, "w").write(harness)
    cc = subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", c, "-o", exe], capture_output=True, text=True)
    if cc.returncode:
        print("FAIL: the parser does not compile on the host\n" + cc.stderr); sys.exit(1)
    out = subprocess.run([exe] + [r for r, _, _ in CASES], capture_output=True, text=True).stdout

ok = True
for (reply, want_text, want_acts), got in zip(CASES, out.split("\x03")):
    text, acts = got.split("\x01")
    acts = [a for a in acts.split("\x02") if a]
    if text != want_text or acts != want_acts:
        print("FAIL: %r -> %r %r, want %r %r" % (reply, text, acts, want_text, want_acts)); ok = False
if ok:
    print("PASS: %d replies: [[note]] and [[led blink]] are stripped and run, anything else stays as text" % len(CASES))
sys.exit(0 if ok else 1)
