# Joshua Tree

Freestanding i386 kernel (renamed from `os` in Sep 2026). No libc, no
dependencies beyond clang, ld.lld and qemu.

- `make` links `kernel.elf`. `make run` boots it. `./check.sh` is the only
  automated boot test.
- Cross-compiles with stock Apple clang via `-target i386-unknown-none`. No
  cross-toolchain needed; do not add one.
- Full subsystem map: `docs/ARCHITECTURE.md`. Plan and verification notes:
  `docs/roadmap.md`. The design rules (icons, type, colour, dock, motion) are one
  page, `docs/DESIGN.md`, and `tools/checks/design-doc-check.py` fails when a
  number in it stops matching the source.

## Verification rules

- `check.sh` asserts on raw VGA memory through the QEMU monitor, because a
  headless `screendump` renders black even though the kernel is running
  fine. It only proves "boots without crashing." Subsystem correctness
  (paging, disk I/O, context switches) needs manual verification against
  actual artifacts (disk images, flat binaries), not `check.sh` alone.
- A pixel-level screenshot is possible only when a session has this
  Mac's own attached display: launch `-display cocoa` (the same window
  the JoshuaTree.app launcher in `nulljosh/joshuatree-monitor` opens), bring it frontmost, then use
  macOS's own `screencapture` CLI. QEMU's own `screendump` is broken
  headless and should not be used for visual verification.
- **HEADLESS ONLY. Never open a visible QEMU window and never open a
  browser to verify anything.** Every check in this repo must run without
  popping a window.
- The QEMU monitor's `sendkey ret` does not reliably deliver Enter to this
  kernel's keyboard driver (a harness limitation, not a kernel bug).
  Typing individual letters works. Verifying a command's *output*
  needs the boot-time direct-call trick instead: temporarily call the
  function from `kmain` before `clear()`, dump VGA memory, revert.
- After any rename/move/delete, run `tools/checks/check-refs.sh`. It scans
  `docs/roadmap.md`, `CLAUDE.md` and `docs/ARCHITECTURE.md` for backtick file
  paths and fails if one points at something that no longer exists.
- `make hooks` (once, per clone) points this repo's own push hook at
  `tools/hooks/pre-push` (fast, no QEMU window: check-refs plus a
  compile), overriding this machine's global `core.hooksPath` for this
  repo only. Skip a single push with `git push --no-verify`.
- Every shipped feature needs a permanent, discriminating regression
  test, not just a one-off screenshot: either a new shell command (the
  `heaptest`/`tasktest`/`ring3test` pattern in `kernel.c`) or a
  `tools/checks/*-check.{sh,py,mjs}` script. Prove the test fails when
  the fix is reverted and passes when it's restored.
- `docs/ARCHITECTURE.md`'s Files table should have a row for every
  real `.c` file and header-only subsystem in the repo. Add a row in the
  same pass that adds the file.
- `.github/workflows/check.yml` runs the regression suite on every push.
  Keep it current as real gaps turn up.
- `gh release create`/`edit` gets substantive release notes (what shipped,
  the root cause if there was one, how it was verified), not a placeholder
  line. `docs/roadmap.md`'s own entry for the version is usually the source.
- Before tagging a MINOR (`x.Y.0`) release: re-run the full regression
  suite, look for any security-relevant boundary the new code touches
  (user input, untrusted network data, a privilege edge), and take one
  screenshot to confirm the new chrome matches the house theme below.
  Skip this for PATCH releases.

- **Docs stay at 100%.** Every source file the progress graph counts needs a row in `docs/ARCHITECTURE.md` in the same PR that adds it. The landing graph shows the number, so a dip is public.

## Writing docs

Docs are for people first. Plain words, short sentences, one idea per
sentence. A roadmap entry says what changed and why it matters in two
or three lines, then names the check that proves it. Put the long story
(root cause, what was tried, file and line detail) in the commit message
or the PR, not in `docs/roadmap.md` or `docs/ARCHITECTURE.md`. Name a
file, function or flag only when the reader has to go there. No walls of
text. Read it back as someone who has never seen the code.

## Versioning

`VERSION` holds `MAJOR.MINOR.PATCH`, semver since v31 (`v1`-`v30` keep
their original tags, not rewritten). Bump MINOR for a new capability,
PATCH for a fix that adds no capability, MAJOR only for an actual
breaking change (none yet: this kernel has no external callers to
break). `0.x.y` is deliberate: nothing here has a stable contract yet for
a 1.0.0 to mean something.

Each version gets a bare `X.Y.Z` tag and GitHub release, cut by
`.github/workflows/release.yml` on merge (from 1.0.13 on; earlier tags
keep their `jt-v` prefix, not rewritten). No letters in version names. Each MAJOR version gets its own codename once it
ships (the Ubuntu/macOS relationship: "Joshua Tree" stays the one project
name, the codename is a per-major label alongside it). No codename exists
yet for 1.0.0.

## Theme

Keep the Satellite wallpaper in color. The former engraving filter made it black and white, while the cream, hatched landing page felt too much like paper or a blueprint. The landing page now uses an off-white surface (`#faf8f4`), near-black ink, one terracotta accent (`#b5502c`), simple rules and rounded cards, with no teal, purple or gradients. Dark mode swaps to a warm near-black (`#141311`). The OS's own look is written down in `docs/DESIGN.md`. The logo is a crayon scribble of one Joshua tree (2.25, replacing the engraved tree): thin wobbly lines for the trunk, arms and ground, a dense looping scribble for each tuft, black ink on `#faf8f4` (an off-white line on `#141311` in dark mode), no fill, no shading. `tools/gen/logo.py` draws every copy. `landing/logo.svg` carries a light crayon grain; `landing/mark-bold.svg` is the clean heavy copy for 16 to 48 px (favicon, menu bar, About). `landing/mark.png` is for small sizes and `landing/badge.png` for large artwork. The VGA boot text is the kernel's actual black-on-gray output.

## Landing page / v86 demo

`landing/index.html` (deployed to joshuatree.heyitsmejosh.com) embeds a
live v86 (JS/wasm x86 emulator) instance booting this exact `kernel.elf`
in-browser (`landing/v86/embed.js`), not a recording.
Keyboard/mouse capture is gated behind click/tap-to-focus, because v86
attaches its own input listeners to `window` globally. An idle timer (8s)
starts a scripted autoplay tour and hands control back the instant a
visitor clicks or types.

`landing/v86/kernel.elf` is a build artifact, not a committed file (it
used to be committed, which meant every kernel PR conflicted on it).
`make kernel.elf` (or `make run`) produces it locally, same as the
root `kernel.elf`; the Makefile's rule copies one to the other on every
build. `.github/workflows/deploy.yml` builds it fresh from source before
deploying, so the live demo always serves the real binary.

`.github/workflows/deploy.yml` runs `wrangler deploy` on every push to
`main` that touches `landing/**`, `wrangler.toml`, or the kernel source
(`kernel/**`, `drivers/**`, `boot/**`, `lib/**`, `user/**`, `Makefile`),
using the `CLOUDFLARE_API_TOKEN` repo secret (already set). This is this
repo's exception to the fleet-wide "a push deploys nothing" rule.

`progress.svg` is a line chart of hand-authored kernel/driver lines over
commit history, generated by `tools/gen/progress.sh` from `git log`.
Regenerate it (`./tools/gen/progress.sh`) after any commit that should
move the line, and redeploy.

`python3 tools/gen/landing-roadmap.py` updates the landing page's "Where
it's going" card from the first three open, numbered, bold-titled items
in `docs/roadmap.md`'s Session task queue. Run `python3
tools/checks/landing-roadmap-check.py` to check the generator itself, and
`python3 tools/gen/landing-roadmap.py --check` to check the saved HTML is
current. Deployment runs the generator automatically.

## Task routing

Main session directs (Sonnet/Opus, Fable for anything privilege/security/
exact-layout shaped: wire protocols, register frames, memory-model
changes). For mechanical, well-scoped work with a known-correct shape
(a UI tweak, a glue fix, a bug with an obvious pattern to follow, tagged
`[Haiku]` in `docs/roadmap.md`), spawn a Haiku subagent with full
sub-instructions (root cause, fix, required test, verification, commit/
push) instead of doing it directly.

## The loop

Current resume state: `docs/LOOP-HANDOFF.md`. Its PR approval requirement remains in force.

This project has no finish line. Each pass:

1. Check for a direct request first; fall back to `docs/roadmap.md`'s open
   queue only when there isn't one. Re-read `docs/roadmap.md` fresh from disk
   every pass, never from memory: other agents can be committing to it
   in parallel.
2. Absent a direct request, standing focus is typeface/font rendering
   and icon sharpening, re-checked against real screenshots each pass.
3. Pull technique from prior art (OSDev wiki, xv6, ToaruOS, Linux/BSD
   source) for anything with a known-solved shape, rather than
   reinventing a wire protocol or register sequence.
4. Verify against an artifact before calling it done: `check.sh`, a
   screenshot, a disk image. Run `tools/checks/check-refs.sh` after any
   rename/move/delete.
5. Ship it: commit, open the PR as a draft (`gh pr create --draft`), so
   pushes while it's still being iterated on never trigger CI or a
   failure email. Run `tools/ci-local.sh` before asking for a real CI
   run; only once it passes, `gh pr ready <N>`. `wrangler deploy` for
   landing-page-only changes (or let `deploy.yml` do it), bump `VERSION`
   for kernel work, one clear TLDR back, then pick up step 1 again.

Constraints in this file and in `docs/roadmap.md`'s model-routing legend get
tightened in place as gaps turn up, not left to drift.

## ARM64 port

`arch/arm64/` is a second, separate build for the Raspberry Pi (plan and milestones in `docs/ARM64.md`). `make -C arch/arm64 run` boots it under `qemu-system-aarch64`; `tools/checks/arm64-m0-check.py` proves it. Nothing in the i386 Makefile changes.

## Naming

"Joshua Tree" is the project's name, full stop. The earlier "Leopard
Gecko" idea is dropped and not coming back. Not to be confused with gato
(`~/Documents/Code/gato`), a separate macOS voice app.

## Debug QEMU runs always get a timeout

Never start QEMU with `-d int` (or any `-d` trace) and `-D <file>` without a
`timeout` in front of it. On 2026-09-29 one was left running for two days,
grew `/tmp/fbq.log` to 5GB, filled the disk and took down everything else on
the Mac twice. `timeout 60 qemu-system-i386 ... -d int -D /tmp/x.log`, then
delete the log when you're done reading it.
