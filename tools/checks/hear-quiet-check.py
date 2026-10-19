#!/usr/bin/env python3
"""A bare `hear` on the Pi records until you stop talking (docs/GUIDE.md).

Host-only, no hardware. Compiles the real heard_end block out of arch/arm64/ask.c with clang and feeds it 16 kHz
sample streams in 100 ms blocks, the way hear_command does: silence never ends it (the ten second cap does), hiss under
the threshold never counts as speech, speech then 1.2 s of quiet ends it right there, and a short pause mid-sentence
does not. --mutate drops the quiet counter's reset on speech, and the pause case must then fail.
"""
import os, re, subprocess, sys, tempfile

src = open("arch/arm64/ask.c").read()
m = re.search(r"/\* pi-hear-quiet begin.*?/\* pi-hear-quiet end \*/", src, re.S)
if not m:
    print("FAIL: no pi-hear-quiet block in arch/arm64/ask.c"); sys.exit(1)
block = m.group(0)
if "--mutate" in sys.argv:
    cut = block.replace("*spoke = 1; *quiet = 0;", "*spoke = 1;")
    if cut == block: print("FAIL: the mutation found no reset to drop"); sys.exit(1)
    block = cut

HZ = 16000
CASES = [   # name, [(seconds, amplitude), ...], sample where it must stop (None: never)
    ("silence", [(10, 0)], None),
    ("hiss under the threshold", [(10, 900)], None),
    ("speech then quiet", [(1.0, 8000), (2.0, 0)], int(1.0 * HZ + 1.2 * HZ)),
    ("a pause mid-sentence", [(0.5, 8000), (0.8, 200), (0.7, 6000), (3.0, 0)], int(2.0 * HZ + 1.2 * HZ)),
]
harness = """#include <stdio.h>
#include <stdlib.h>
#define HEAR_HZ 16000
%s
int main(int argc, char **argv) {
    /* argv: pairs of seconds and amplitude; prints the sample count at which heard_end said stop, or -1 */
    unsigned total = 0; static short pcm[HEAR_HZ * 20];
    for (int a = 1; a + 1 < argc; a += 2) {
        unsigned n = (unsigned)(atof(argv[a]) * HEAR_HZ); int amp = atoi(argv[a + 1]);
        for (unsigned i = 0; i < n; i++) pcm[total + i] = (short)((i & 1) ? amp : -amp);   /* a square wave at that level */
        total += n;
    }
    int spoke = 0; unsigned quiet = 0, got = 0;
    while (got < total) {   /* 100 ms blocks, as hear_command takes them */
        unsigned n = total - got < HEAR_HZ / 10 ? total - got : HEAR_HZ / 10;
        got += n;
        if (heard_end(pcm + got - n, n, &spoke, &quiet)) { printf("%%u\\n", got); return 0; }
    }
    printf("-1\\n");
    return 0;
}
""" % block
fails = []
with tempfile.TemporaryDirectory() as d:
    c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
    open(c, "w").write(harness)
    cc = subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", c, "-o", exe], capture_output=True, text=True)
    if cc.returncode:
        print("FAIL: heard_end does not compile on the host\n" + cc.stderr); sys.exit(1)
    for name, parts, want in CASES:
        args = [str(x) for p in parts for x in p]
        got = int(subprocess.run([exe] + args, capture_output=True, text=True, timeout=10).stdout)
        # it stops at the end of the 100 ms block that holds the 1.2 s mark
        ok = got == -1 if want is None else want <= got < want + HZ // 10
        print(("  ok: " if ok else "  FAIL: ") + "%s: stopped at %s, want %s" % (name, got if got >= 0 else "never", want if want else "never"))
        if not ok: fails.append(name)
if "--mutate" in sys.argv:
    if "a pause mid-sentence" in fails: print("PASS: --mutate: without the reset a mid-sentence pause cuts you off, as it must"); sys.exit(0)
    print("FAIL: --mutate: the build without the reset passed"); sys.exit(1)
if fails:
    print("FAIL: %d of %d hear cases" % (len(fails), len(CASES))); sys.exit(1)
print("PASS: a bare hear stops 1.2 s after you stop talking, never on silence or hiss, and not on a short pause")
