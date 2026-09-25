# Joshua Tree loop handoff (2026-09-25, late evening)

## What the loop is

Keep building Joshua Tree headlessly toward 2.0. Stability, usability and performance come before feature count. Merge on green CI, never ask. Sonnet subagents max 4 at 15 minutes each; code changes only via visible subagents, never background shells. Subagents run every related check before pushing a PR. Fix CI, never mute notifications. Main session watches CI, merges, handles next item. Stop at 90% session or weekly usage with /checkpoint.

A+ is the bar: everything spotless, every line in Joshua's voice mixed with Apple and Steve Jobs. No lines of code or check counts as public metrics.

## Where things stand

Main is 1.5.3+. Shipped eight releases: 1.3.0 Notes text selection and Chat showing every Samantha tool, 1.3.1 Calendar padding and menu color (CI 12 to 8 min), 1.4.0 engraved tree and smooth corners, 1.4.1 landing unified on one design system with Meet Samantha and app sections plus privacy page (8s weather timeout), 1.4.2 landing QA and stb_truetype font engine, 1.5.0 Notes on real typeface (six fonts, 12 to 200), 1.5.1 Terminal on same engine, 1.5.2 Files List/Icons views, 1.5.3 landing A+ pass. Added userland libjt (string, malloc arena, printf, FILE) and built WC.BIN; found clang malloc-builtin miscompile (version check in pre-push). In flight: 1.5.6 boot splash with engraved tree, 1.5.7 landing screenshots with new type, 1.5.8 demo canvas 1:1 device pixels (pixely fix), 1.5.9 antialiased Stocks lines.

## Next, in order

0. Rewrite docs in plain English: ARCHITECTURE.md, SYSCALL-ABI.md, WHITEPAPER.md intros. Joshua says they still read like dev notes. Every row one or two short sentences a 20-year-old follows, what it does and why, jargon only where unavoidable and then explained. Keep file names and one-row-per-file for coverage. Voice: tripwire README.
1. Land 1.5.6 through 1.5.9 (boot splash, landing screenshots, pixel-perfect demo, antialiased charts).
2. UI text (menu bar, titles, labels) onto the TTF engine. Polish pass.
3. Browser demo lag profiling in v86. Measure frame time.
4. Put WC.BIN on the disk image and port hello, note to libjt.
5. ELF loading from disk, apps outside the kernel, tiny C compiler (2.0 spine).
6. Make chattools-check.py's marker waits load-tolerant. Under 4 parallel QEMU shards in tools/ci-local.sh it flaked once (timeout waiting on a chattool= marker that shows up fine standalone) -- give the waits a longer deadline under shard load instead of the fixed 20s, so a slow shared runner doesn't read as a real regression.

## Restart prompt

```
/loop Joshua Tree loop toward 2.0: docs rewrite first (ARCHITECTURE, SYSCALL-ABI, WHITEPAPER plain English), then land 1.5.6-1.5.9, UI text on TTF, v86 lag profile, WC.BIN on disk, ELF loading. Merge on green, visible subagents 15 min max, fix CI never mute, every check before push, A+ bar, headless only, stop at 90% session or weekly with /checkpoint.
```
