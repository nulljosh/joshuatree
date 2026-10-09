#!/usr/bin/env python3
"""Samantha's Pi actions: [[note TEXT]], [[say TEXT]], [[led blink]], [[open APP]], [[browse URL]], [[calc EXPR]] and
[[status]] at the end of an answer (docs/AGENT.md).

Host-only, no hardware. Compiles the real take_actions block out of arch/arm64/ask.c with clang and feeds it replies:
known markers are stripped and recorded, an unknown marker line is recorded as kind 0 (the kernel logs and ignores
it), a fifth action, a long one or one inside a sentence stays in the printed text, and [[browse]] takes http and
https only. Also checks the relay's system prompt names every action and the ask-before-changing-files rule.
"""
import os, re, subprocess, sys, tempfile

src = open("arch/arm64/ask.c").read()
m = re.search(r"/\* pi-actions begin.*?/\* pi-actions end \*/", src, re.S)
if not m:
    print("FAIL: no pi-actions block in arch/arm64/ask.c"); sys.exit(1)
relay = open("tools/claude-relay/relay.py").read()
prompt = re.search(r"SAMANTHA = \((.*?)\)\n", relay, re.S)
ACTIONS = ["[[note TEXT]]", "[[say TEXT]]", "[[led blink]]", "[[open APP]]", "[[browse URL]]", "[[calc EXPR]]", "[[status]]"]
missing = [a for a in ACTIONS if not prompt or a not in prompt.group(1)]
if missing or "ask first" not in prompt.group(1):
    print("FAIL: the relay's SAMANTHA prompt lacks %s or the ask-first rule" % (missing or "nothing")); sys.exit(1)

LONG = "[[note " + "x" * 80 + "]]"
CASES = [   # reply -> printed text, actions as "kind:text"
    ("Done.\n[[note hello there]]", "Done.", ["1:hello there"]),
    ("Blinking.\n[[led blink]]\n", "Blinking.", ["2:"]),
    ("Hi.\n[[reboot]]", "Hi.", ["0:reboot"]),
    ("Hi.\n" + LONG, "Hi.\n" + LONG, []),
    ("Say [[led blink]] to blink.", "Say [[led blink]] to blink.", []),
    ("Ok.\n[[note a]]\r\n[[led blink]]  ", "Ok.", ["1:a", "2:"]),
    ("Plain answer\nover two lines", "Plain answer\nover two lines", []),
    ("[[note ]]", "", ["0:note "]),
    ("Look.\n[[say hi there]]\n[[open Calculator]]\n[[calc 2+2]]\n[[status]]", "Look.", ["3:hi there", "4:Calculator", "6:2+2", "7:"]),
    ("Page.\n[[browse https://example.com/a]]\n[[browse http://10.0.2.2:80/x]]", "Page.", ["5:https://example.com/a", "5:http://10.0.2.2:80/x"]),
    ("No.\n[[browse ftp://x]]\n[[browse example.com]]", "No.", ["0:browse ftp://x", "0:browse example.com"]),
    ("Five.\n[[note 1]]\n[[note 2]]\n[[note 3]]\n[[note 4]]\n[[note 5]]", "Five.\n[[note 5]]", ["1:1", "1:2", "1:3", "1:4"]),
    ("[[statusx]]\n[[open]]", "", ["0:statusx", "0:open"]),
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
    print("PASS: %d replies: the seven actions are stripped and recorded, four per turn, unknown ones recorded to ignore, the rest stays as text" % len(CASES))
sys.exit(0 if ok else 1)
