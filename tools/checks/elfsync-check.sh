#!/bin/bash
# Real recurring gap: landing/v86/kernel.elf is a checked-in binary copy,
# not a build artifact, so a docs-only or landing-only pass never touches
# it and it silently drifts behind the real kernel.elf (caught live via
# md5, the demo was missing several real versions' worth of features).
#
# First version of this check compared a CI-fresh build's md5 against the
# checked-in copy directly, a real bug found within a day: clang/lld builds
# aren't bit-reproducible across machines/toolchain versions (Ubuntu CI vs
# whoever's Mac built the checked-in copy), so that false-failed on a
# genuinely in-sync tree. Real fix: compare git history instead of bytes,
# was `landing/v86/kernel.elf` committed at or after the last commit that
# actually touched kernel/driver/boot source, not whether two different
# compilers produced byte-identical output.
#
# Makefile dropped from the watched set (real regression, hit twice same
# night): a Makefile edit that only fixes a generator rule's own path
# string (no CFLAGS/source-list change) produces a byte-identical
# kernel.elf, so there's nothing to actually commit, every retry of
# "cp kernel.elf landing/v86/kernel.elf" is a no-op git sees as "nothing
# to commit" and the check fails forever no matter what's done. A real
# Makefile change that DOES affect the build (new source file, changed
# flags) always lands alongside real edits under kernel/drivers/boot/lib
# in the same commit anyway, so this still catches the case that matters.
#
# Needs full history: CI checks out with fetch-depth 0 for exactly this.
set -eu
cd "$(dirname "$0")/../.."

src_commit=$(git log -1 --format=%H -- kernel/ drivers/ boot/ lib/)
elf_commit=$(git log -1 --format=%H -- landing/v86/kernel.elf)

if [ -z "$src_commit" ] || [ -z "$elf_commit" ]; then
    echo "FAIL: no commit found for the kernel sources or landing/v86/kernel.elf"
    echo "(a shallow clone will do this: CI must check out with fetch-depth: 0)"
    exit 1
fi

if ! git merge-base --is-ancestor "$src_commit" "$elf_commit" 2>/dev/null; then
    echo "landing/v86/kernel.elf (last touched $elf_commit) predates real source changes (last touched $src_commit)"
    echo "run: cp kernel.elf landing/v86/kernel.elf"
    exit 1
fi

echo "PASS: landing/v86/kernel.elf ($elf_commit) is at or after the last real source commit ($src_commit)"
