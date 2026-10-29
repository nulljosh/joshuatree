#!/usr/bin/env python3
"""plankc's text advances (6.97.0), from user/libjt/aafont.h, so a Plank window's text lays out exactly as libjt's does.

The kernel's JT_SYS_TEXT2 draws each byte where the app says and moves the pen by the app's own advance, so plankc's
runtime (user/plankc/rt.pk, jtw_text) needs the 1x advance of every body and bold glyph from space to tilde, then the
degree sign. They go into rt.pk between the jtw-adv markers as one string a face, each advance plus 48 so the string is
printable. --check exits 1 when rt.pk's block is not what aafont.h gives (tools/checks/plankc-check.py runs it).

    python3 tools/gen/plank-adv.py [--check]
"""
import os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../.."))
src = open(os.path.join(ROOT, "user/libjt/aafont.h")).read()
mono = open(os.path.join(ROOT, "user/libjt/aamono.h")).read()
text_h = open(os.path.join(ROOT, "user/libjt/text.h")).read()

def advances(face):
    body = re.search(r"aa_%s_glyphs\[\] = \{(.*?)\n\};" % face, src, re.S).group(1)
    adv = [int(m.group(1)) for m in re.finditer(r"\{[^}]*?,(\d+)\}", body)]
    assert len(adv) >= 96, (face, len(adv))
    return "".join(chr(48 + a) for a in adv[:96])

def ascent(face):
    return int(re.search(r"#define AA_%s_ASCENT (\d+)" % face, src).group(1))

for f in ("BODY", "BOLD"):
    assert all(32 <= ord(c) <= 126 and c not in '"\\(' for c in advances(f)), f   # plain characters in a Plank string
block = ("# jtw-adv:start (tools/gen/plank-adv.py writes this block from user/libjt/aafont.h; do not edit by hand)\n"
         "fn rt_adv(face: int) -> str {   # 1x advances, space to tilde then the degree sign, each plus 48\n"
         '  if face == 1 { return "%s" }\n'
         '  return "%s"\n'
         "}\n"
         "fn rt_ascent(face: int) -> int {   # face 3 is mono (6.98.0)\n"
         "  if face == 3 { return %d }\n"
         "  if face == 1 { return %d }\n"
         "  return %d\n"
         "}\n"
         "fn rt_mono_adv() -> int { return %d }\n"
         "# jtw-adv:end\n") % (advances("BOLD"), advances("BODY"),
                              int(re.search(r"#define AA_MONO_ASCENT (\d+)", mono).group(1)), ascent("BOLD"), ascent("BODY"),
                              int(re.search(r"#define JT_MONO_ADV (\d+)", text_h).group(1)))

rt = os.path.join(ROOT, "user/plankc/rt.pk")
text = open(rt).read()
m = re.search(r"# jtw-adv:start.*?# jtw-adv:end\n", text, re.S)
if not m: print("plank-adv: no jtw-adv block in rt.pk"); sys.exit(1)
if "--check" in sys.argv:
    ok = m.group(0) == block
    print("plank-adv: rt.pk's advances %s user/libjt/aafont.h" % ("match" if ok else "do NOT match"))
    sys.exit(0 if ok else 1)
open(rt, "w").write(text[:m.start()] + block + text[m.end():])
print("plank-adv: wrote rt.pk's advances from user/libjt/aafont.h")
