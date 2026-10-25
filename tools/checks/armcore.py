"""The Pi kernel core's source as one text: arch/arm64/main.c and every part cut out of it (the Makefile's MAINSRC),
plus kmain.h. Checks that read the core's source use this, so a split that moves code between the parts never breaks
them. core_source() is the joined text; core_files() the paths, main.c first; part_with(text) the file holding text."""
import os, re
ARCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "arch", "arm64")


def core_files():
    mk = open(os.path.join(ARCH, "Makefile")).read()
    parts = re.search(r"^MAINSRC = (.*)$", mk, re.M).group(1).split()
    return [os.path.join(ARCH, p) for p in parts] + [os.path.join(ARCH, "kmain.h")]


def core_source():
    return "\n".join(open(p).read() for p in core_files())


def part_with(text):
    for p in core_files():
        if text in open(p).read(): return p
    return None
