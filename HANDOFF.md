# Handoff: "Every app's main action, headless" cascade failure (Linux CI only)

## First failing app
**Homeqi** (app index 16), immediately after Quotes (index 11) finishes its
action and closes. Confirmed from GH Actions run 36443048107, shard 0, retry
attempt (the one that reached furthest): apps tested in this exact order:
Notes, Reminders, Calendar, Terminal, Samantha, Search, Calculator, Stocks,
Epiphany, Contacts, Lexly, **Quotes** (opened yes, action yes) -> **Homeqi**
(opened NO) -> Sparkjar, Toroid, Keyrate, Bookrank, Fieldbook, Plan, Curbfind,
Portfolio, Activity, Files, Mail, Weather, Trash: every single one after
Quotes fails "never opened a window", including apps with no VFS dependency
at all (Files, Mail, Weather, Trash are open-only, no seeding).

Serial evidence (from `gh run view 36443048107 --repo nulljosh/joshuatree
--log-failed`): Quotes' own run is clean —
```
ring3app: launching QUOTES.BIN at ring 3
exec: started ring-3 task from VFS
syscall: window opened for ring-3 task
syscall: write(1) from ring 3: quotes: ring-3 window 832x450
syscall: write(1) from ring 3: quotes: pick 1 right
syscall: write(1) from ring 3: quotes: closed
syscall: exit from ring 3
syscall: window released, task gone
ring3app: QUOTES.BIN exited 0, window torn down, desktop alive
```
Toroid (tested later) ALSO ran clean in the same log (opened, action,
closed, exited 0) — so it isn't "ring-3 apps specifically" that break, it's
everything system-wide after Quotes' desktop repaint.

## Local (macOS) result: PASSES clean, all 26/26
Rebuilt kernel.elf on this worktree, ran `tools/checks/feature-drive.py`
directly (not through ci-suite.sh) with no other qemu on the port. All 26
PNGs written to /tmp/jt-fd-out, every app opened, Quotes/Toroid/Keyrate/
Calculator (the 4 ring-3 binaries) all seeded and ran fine. This confirms
the bug is Linux-runner-specific, not a real logic bug an x86 CPU would hit
identically everywhere — almost certainly a timing/race condition in how
GitHub's Linux QEMU schedules the ring-3 task teardown vs. the desktop's
next repaint, exposed only under different host scheduling/CPU emulation
speed than on this Mac (same class as the 1.7.8 Linux-only bss-margin bug
mentioned in the brief).

## Suspected cause (not confirmed)
`tools/checks/feature-drive.py`'s qemu invocation has **no `-hda`/`-drive`**,
so `fat_mount()` always fails headless and the active VFS backend falls back
to **ramfs** (`kernel/kernel.c` ~9785-9819), which pre-seeds `README.TXT` +
`NOTES.TXT`. `drivers/ramfs.c` caps at `RAMFS_MAX_FILES 8`. Every ring-3 app
(`kernel/ring3app.c` RING3_APPS: Keyrate, Toroid, Calculator, Quotes) seeds
its `.BIN` into VFS on first launch via `vfs_write_file`. Quotes is the 4th
ring-3 binary added (1.7.14) — it's plausible the CI run's file count (demo
seeds + Notes/Reminders saves + 4 ring-3 .BIN files) sits right at or over
the 8-file ramfs cap, and a failed `vfs_write_file` deeper in the desktop's
own bookkeeping (not just app-seeding) wedges the GUI for every later
`gui_launch_from_dock` call. This is a hypothesis, NOT verified — I did not
find where a full ramfs would cascade into "no app ever opens a window
again" rather than just failing that one app's own seed. It also doesn't
cleanly explain why Files/Mail/Weather/Trash (no VFS writes at all) fail
too, unless the desktop's per-frame repaint path itself touches VFS
(possible — worth checking `gui_launch_from_dock` / desktop autosave code
for any vfs_write_file call that isn't app-specific).

Alternative unverified suspect: a timing race in `exec_user`'s
`while (task_used(id)) yield();` blocking wait (kernel/exec.c:141) — on a
slower/faster Linux CI QEMU, the desktop's post-exit repaint could run
before task teardown (`syscall_release_task`) has fully released the
window/input focus, leaving the GUI in a state where the *next* app's
`gui_launch_from_dock` silently no-ops. This would explain why it's the
next app after Quotes that fails, on Linux only, regardless of VFS.

## What was run
- `git -C /tmp/jt-quotes3 fetch -q` (up to date with origin/feat/ring3-quotes)
- `make kernel.elf -j4` (rebuilt clean)
- `lsof -nP -iTCP -sTCP:LISTEN | grep qemu` before the run (one stray listener
  on port 4461, unrelated to this check's port 4621 — did not interfere)
- `python3 tools/checks/feature-drive.py /tmp/jt-fd-out` run directly,
  standalone — PASSED, 26/26 apps opened (macOS)
- `gh run view 36443048107 --repo nulljosh/joshuatree --log-failed` — pulled
  the Linux CI failure evidence above (did not need to download the
  shard-0-evidence artifact separately; --log-failed had the serial dump
  inline)

## Next command
Reproduce the exact CI conditions locally if possible (ubuntu qemu via
`act` or a Linux VM/Docker), OR instrument `ring3app_launch` and the
desktop repaint path with a serial print of `vfs_current_name()` +
approximate ramfs file count right before/after each app launch, push that
as a temporary diagnostic commit, and read the next CI run's serial log to
confirm or rule out the ramfs-cap hypothesis. If confirmed, the real fix is
raising `RAMFS_MAX_FILES` (drivers/ramfs.c:4) or having demo/test seed
files evicted/reused rather than accumulating — never loosening the check
itself.

## Status
No code changes made. Nothing pushed yet beyond this file (about to push
as WIP). ci-suite.sh and feature-drive.py are unmodified.
