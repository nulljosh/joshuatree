# Blueprint: where the OS goes after 1.0

1.0 shipped a real desktop: 22 apps, a compositor-free GUI, ring 3 with a
frozen syscall ABI that nothing uses yet. This is the plan for the next
structural shift: apps as real user processes, a window server, fault
isolation that actually protects the kernel from a buggy app. It is
research-grounded (SerenityOS, ToaruOS, Haiku, Redox, Essence, xv6,
KolibriOS) and phased as small PRs, each keeping headless CI green.

## Where we are, measured

- `kernel/kernel.c` is 9,079 lines. Most GUI apps (Notes, Reminders,
  Calendar, Mail, Contacts, Chat, Calculator, Stocks, Epiphany, Search,
  Settings) still live in it; 11 apps have already moved out to
  `drivers/app_*.h`-style headers (Quotes, Toroid, and others per
  `docs/ARCHITECTURE.md`'s Apps section).
- Ring 3 exists (`kernel/ring3.c`, `kernel/ring3.h`, `kernel/ring3_asm.S`)
  with a real lifecycle: user page tables, a TSS-backed ring-3-to-ring-0
  stack switch, preemption under the same scheduler as kernel tasks. 9
  syscalls are wired (`kernel/syscall.h`): EXIT, READ, WRITE, OPEN, CLOSE,
  TIME, LSEEK, GETPID, SCHED_YIELD. `docs/SYSCALL-ABI.md` freezes v1.
- Fault isolation for a ring-3 *task* already exists at the exception
  level: `kernel/idt.c` checks CPL from the pushed CS selector, and a
  fault from ring 3 prints the task's name, reaps it, and the kernel
  keeps running; a ring-0 fault halts. `ring3test fault` proves this.
  What does not exist is any GUI app running where that protection would
  ever fire: every GUI app runs in ring 0, sharing the kernel's address
  space, so an app bug is a kernel bug today.
- No app has a syscall for what a GUI app actually needs: no framebuffer
  handle, no input event stream, no shared memory. The v1 ABI is a CLI
  contract (`user/hello.c`, `user/note.c`), not a GUI one.
- `drivers/window.c` is the one render-target abstraction every draw call
  goes through, but it has no per-window backing store and no
  compositor: `docs/roadmap.md`'s own Multi-window section names this gap
  directly ("Per-window backing stores... A compositor with damage
  tracking... Input routing by focus").

## Comparable systems

| OS | Kernel type | GUI apps run in | Window server / IPC | libc | Repo layout | Build | Tests | Build order (roughly) |
|---|---|---|---|---|---|---|---|---|
| [SerenityOS](https://github.com/SerenityOS/serenity) | Monolithic, x86_64, preemptive | Userland processes | `WindowServer` service, custom `IPC::Connection` (compiler-generated `.ipc` stubs over local sockets) | Own `LibC`, POSIX-ish | `Kernel/`, `Userland/Libraries/`, `Userland/Services/`, `Userland/Applications/` | CMake + Ninja, Lagom (host build of the libs for fast iteration) | CI runs Lagom unit tests on the host, plus a boot-to-desktop smoke test in CI | Kernel + LibC first, then a shell, then WindowServer + LibGUI, then apps one at a time |
| [ToaruOS](https://github.com/klange/toaruos) | Hybrid modular ("Misaka"), x86_64/AArch64 | Userland processes, dynamically-linked ELF | `Yutani` compositor, custom `PEX` (packet exchange) IPC over a pipe-like device | Own cleanroom libc, incomplete on purpose | `kernel/`, `libc/`, `lib/` (userspace libs), `apps/` | Make + a Kuroko script (`auto-dep.krk`) that generates per-app Makefiles from `#include` graphs | Boots to a ramdisk image in QEMU in CI; largely manual/visual otherwise | Kernel + libc + a shell first, then Yutani + a terminal, then a compositor-aware toolkit, then apps |
| [Haiku](https://www.haiku-os.org/) (BeOS lineage) | Hybrid kernel (fork of NewOS) | Userland processes, client/server | `app_server`, message-based IPC (`BMessage`s over ports); every app is a client of `app_server`, which is a client of the kernel | Own libroot (POSIX + BeOS API) | `src/system/kernel/`, `src/servers/app/`, `src/kits/` (client-side API), `src/apps/` | Jam (custom build tool predating this project) | `haiku-run-tests`, a real test suite plus boot images tested in CI/QEMU | Kernel + libroot first, `app_server` + the interface kit next (this was BeOS's original order too), then apps |
| [Redox](https://redox-os.org/) | True microkernel, Rust | Userland processes | `Orbital` window manager, no bespoke IPC primitive: everything, including drivers and the window manager, is a `scheme` (a namespace mounted at `/scheme/name`) accessed through ordinary file operations | `relibc`, own Rust-first libc | `kernel/` (tiny), `cookbook/` (every userspace package as its own recipe), `programs/` | Custom Rust build + a "cookbook" packaging system, everything else out-of-tree | `redox_tester`/CI boots a QEMU image and runs an integration suite | Microkernel (memory, scheduling, IPC) first, minimal drivers as schemes, then a shell, then Orbital, then ports of real userspace software |
| [Essence](https://github.com/nakst/essence) (nakst) | Monolithic, solo-authored | Userland processes | Compositor built into the kernel/OS core; a documented syscall surface for windows/surfaces, message queues per process | Own libc-like base library, not POSIX | Single repo: `kernel/`, `desktop/`, `apps/`, `drivers/` all in one tree, no separate ports collection | Own build scripts, one-command build | Manual/visual QA plus a scripting/test framework nakst built for UI regression | Kernel + a minimal syscall layer first, then the compositor and a window/message-passing API, then a handful of apps as its own dogfood |
| [xv6](https://github.com/mit-pdos/xv6-public) (MIT teaching OS) | Monolithic, minimal, no GUI at all | N/A — no GUI, console only | N/A | A tiny teaching libc | `kernel/`, `user/` | Make | A handful of shell-driven user programs as the "tests" | Boot, memory, processes, syscalls (fork/exec/wait), a console shell, then the trivial user programs. Relevant here only as the cleanest reference for "how few syscalls (~20) a real fork/exec Unix needs" |
| [KolibriOS](https://kolibrios.org/) | Monolithic, x86, pure assembly | **Ring 0** — apps import the kernel's function table directly, same address space, same ring as the kernel; ring-3 segments exist but every app shares one address space | No window server process: a compositor exists but lives inside the kernel; the "IPC" is direct syscall calls, not messages between processes | No libc: apps call kernel functions directly (or `libGUIc`, a thin C wrapper) | One flat assembly tree, `kernel/`, `programs/` | FASM (flat assembler), extremely fast full-system builds (~seconds) | Mostly manual QEMU boot testing | Kernel + graphics driver + a window manager built into the kernel from day one; apps came after, never separated into their own ring. This is Joshua Tree's current shape today, and Kolibri is the closest live counterexample: it works, but never gets fault isolation between apps, exactly the tradeoff this blueprint moves away from |
| [MenuetOS](http://www.menuetos.net/) | Monolithic, pure assembly (like Kolibri, its ancestor) | Ring 0, same as Kolibri | Kernel-resident window manager | None | Flat tree | FASM | Manual | Same order as Kolibri: kernel, graphics, window manager, apps, all ring 0 from the start |

The pattern across every OS that ships isolation (Serenity, ToaruOS,
Haiku, Redox, Essence) is the same regardless of kernel philosophy
(monolithic, hybrid, or micro): **the kernel exposes a small, stable
surface for framebuffers/surfaces and input, one process runs the
compositor, and every app is a separate address space talking to it over
message-passing, not shared memory it can corrupt.** The two systems that
skip this (KolibriOS, MenuetOS) keep apps in ring 0 permanently, by
design, and both accept "one bad app can take down the desktop" as a
tradeoff for build/run simplicity. Joshua Tree already ships something
KolibriOS and MenuetOS never built: real ring-3 fault isolation
(`ring3.c`, `idt.c`). The gap is that no GUI app uses it. Closing that gap
is this blueprint's spine, not a rewrite of the kernel philosophy.

## Target architecture

```
 ring 0 (kernel)                         ring 3 (user)
+-------------------------------+       +------------------+
| gdt/idt/irq/pic                |       | window server /  |
| pmm/paging/kheap                |       | compositor       |
| task.c (scheduler)               |<----->| (owns the       |
| syscall.c dispatch table         | int   |  framebuffer,   |
|  EXIT READ WRITE OPEN CLOSE      | 0x80  |  damage list,   |
|  TIME LSEEK GETPID SCHED_YIELD   |       |  input queue)    |
|  + new: SURFACE_CREATE           |       +--------+---------+
|         SURFACE_PRESENT                            |
|         INPUT_POLL / INPUT_WAIT           IPC (shared-memory
|         MMAP_SHARED                        surface + a small
| fat.c / vfs.c / net.c / ...              message queue, not
| drivers/window.c (kept, becomes         sockets — no network
|   the server's own backing store)        stack needed for this)
+-------------------------------+                    |
                                          +------------+-----------+
                                          |   ring-3 app processes   |
                                          |  Notes  Calendar  Mail  |
                                          |  Reminders  Chat  ...   |
                                          |  each: own page tables, |
                                          |  own stack, no direct   |
                                          |  framebuffer access,    |
                                          |  reaped on fault        |
                                          +--------------------------+
```

The kernel keeps owning the framebuffer and `drivers/window.c`'s pixel
primitives; it does not move drawing into ring 3. What moves to ring 3 is
app *logic* (state, key/click handling, layout), talking to a window
server process through a handful of new syscalls. The window server can
itself start out as a privileged ring-0 component (it already is, today,
as `gui_run`) and only needs to become a separate ring-3 process once
apps are the thing being isolated from *it*, not from the kernel. That
ordering — isolate apps from the kernel first, then isolate the
compositor from apps — is deliberate: SerenityOS and Haiku both separated
kernel/driver crashes from userland years before they hardened
`WindowServer`/`app_server` itself.

## Phased plan

Each phase is a sequence of small PRs. Every PR must pass `make`,
`./check.sh`, and `tools/checks/ci-suite.sh` headless, no visible QEMU
window, ever (per `CLAUDE.md`).

### Phase 1 — finish extracting in-kernel apps out of `kernel.c`

- **Goal.** Every app that already has GUI-app shape (its own draw/input
  functions) moves to its own `kernel/<app>.h` or `drivers/app_<name>.h`,
  matching the 11 apps already out. `kernel.c` shrinks toward the
  boot/GUI-chrome/dock/window-manager core only.
- **Exit criterion.** `kernel.c` line count drops in the same commit as
  each extraction (measurable, `wc -l`); `tools/checks/qa-gallery.py` and
  `tools/checks/feature-drive.py` pass unchanged after each move, proving
  behavior didn't shift.
- **Size.** One app per PR, small.
- **Model.** [Haiku] — mechanical file-split, the pattern already exists
  11 times in this repo to copy.
- **Precedent.** SerenityOS and ToaruOS both started as one-giant-file
  kernels during early development and split userland pieces out file by
  file as each stabilized; neither did it as one big rewrite. The lesson
  from both: extracting by app boundary (not by "layer") kept each PR
  independently testable, same shape this phase uses.

### Phase 2 — fault isolation for ring-3 tasks (hardening what exists)

- **Goal.** The exception-reap path (`kernel/idt.c`) already isolates a
  ring-3 fault from the kernel. This phase hardens it for GUI-shaped
  workloads: multiple concurrent ring-3 tasks (today `ring3.c` has one
  static code page and one stack page — "one ring-3 task at a time"),
  per-task cleanup of any window-server resources on reap, and a
  regression test that two ring-3 tasks can run and one can fault without
  disturbing the other.
- **Exit criterion.** A new check (`tools/checks/ring3-multi-check.py` or
  similar) starts two ring-3 tasks, kills one with a fault, and proves
  the second's marker keeps advancing and the kernel serial log shows no
  panic. `ring3test fault` still passes unmodified (v1 ABI frozen).
- **Size.** Medium — touches `ring3.c`'s single-code-page assumption and
  `task.c`'s per-task resource teardown.
- **Model.** [Fable] — exactly the class of "subtly wrong still boots
  fine" work `docs/roadmap.md`'s own model-tag rule calls out: a missed
  page-table teardown or a wrong TSS `esp0` swap won't crash on the happy
  path, only under a fault the tests must specifically trigger.
- **Precedent.** xv6's `exit()`/`wait()` teardown path is the textbook
  minimal version of this: free the page table, close every fd, reparent
  children, wake the parent. Redox's microkernel treats "a process died"
  as a first-class scheduler event for the same reason. The lesson: test
  the *cleanup* path explicitly, not just the crash path — that's where
  xv6 and Redox both put their process-death test coverage, and it's
  exactly what's missing from `ring3test fault` today (it proves the
  kernel survives, not that resources were freed).

### Phase 3 — the syscalls a GUI app needs

- **Goal.** Extend `kernel/syscall.h`/`syscall.c` with the minimum surface
  a windowed app needs: a surface/framebuffer handle scoped to one
  window (not the raw physical framebuffer), an input-event syscall
  (`INPUT_POLL` or a blocking `INPUT_WAIT`, delivering key/mouse events
  already filtered to that app's window), and a shared-memory primitive
  (`MMAP_SHARED` mapping a kernel-owned buffer into the caller's address
  space) so pixel data doesn't have to round-trip through `write()`.
  These are v3 additions; v1 stays frozen per `docs/SYSCALL-ABI.md`.
- **Exit criterion.** A new `docs/SYSCALL-ABI.md` v3 section, plus a
  `user/`-style reference program (`user/surface_demo.c`, following
  `user/hello.c`'s and `user/note.c`'s pattern) that opens a surface,
  draws into it via the shared mapping, and calls `SURFACE_PRESENT`; a
  `tools/checks/*-check` boots it headless and confirms real pixels
  landed via `pmemsave`, the same technique `qa-gallery.py` already uses.
- **Size.** Medium-large — new syscalls, new paging work for shared
  mappings, no change to existing ones.
- **Model.** [Fable] — shared-memory mapping between two address spaces
  is exactly the register/page-table-layout class of bug that boots fine
  and corrupts quietly.
- **Precedent.** Essence's syscall surface for windows is deliberately
  small (create surface, blit, message queue) rather than a full POSIX
  `mmap`+`ioctl` story, and nakst has written about keeping it that way
  on purpose so the ABI stays auditable. Haiku's `BWindow`/`BView` API
  hides a much richer IPC (`BMessage`s over ports) behind a small
  syscall-adjacent surface, but the *syscalls themselves* (`_kern_*`) are
  narrow — the richness lives in userland library code on top, not in the
  kernel entry points. Same shape this phase should take: narrow kernel
  syscalls, any convenience API added later lives in a userland shim, not
  the ABI.

### Phase 4 — one pilot app moved to ring 3

- **Goal.** Pick the smallest, most self-contained app — Calculator is
  the obvious candidate (`kernel/calculator.h`, no VFS persistence, no
  network, a recursive-descent parser and a one-line input) — and port it
  to run as a real ring-3 process against Phase 3's syscalls, talking to
  the still-ring-0 window manager for this phase.
- **Exit criterion.** Calculator opens, computes, and closes identically
  from the user's perspective; `tools/checks/feature-drive.py`'s existing
  Calculator case passes unmodified; a new check proves it's really
  ring 3 (reads CPL from a QMP register dump during its run, or crashes
  it on purpose and confirms the kernel survives and reports the task
  name, the same proof `ring3test fault` already establishes for the
  synthetic demo).
- **Size.** Medium — first real end-to-end proof, expect surprises in
  Phase 3's syscalls that only show up under a real app.
- **Model.** [Sonnet] for the app port itself (a scoped feature with a
  clear pattern once Phase 3 lands); [Fable] for anything that touches
  the syscall entry/exit path to make it work.
- **Precedent.** SerenityOS's own early history: the first "real"
  userland program built against `LibGUI` was small and deliberately
  disposable, used to shake out the window-server IPC contract before
  porting anything that mattered. ToaruOS did the same with an early
  terminal before Yutani carried real apps. The lesson both share: the
  pilot's job is to break the new ABI cheaply, not to prove the concept
  works — expect to revise Phase 3's syscalls after this phase, not
  before it.

### Phase 5 — a window server / compositor process

- **Goal.** Move the window-manager logic currently in `kernel.c`'s
  `gui_run` (dock, title bars, snapping, focus, the desktop) into its own
  process, ring 0 or ring 3 depending on what Phase 4 proved is safe, that
  owns `drivers/window.c`'s framebuffer and mediates every app's surface
  through Phase 3's syscalls. This is also where `docs/roadmap.md`'s
  Multi-window section's "per-window backing store" and "damage
  tracking" items get done, since they only make sense once there's a
  real compositor to own them.
- **Exit criterion.** Every app from Phase 4 (just Calculator, at this
  point) renders through the window-server process, not direct
  framebuffer calls from `gui_run`. `tools/checks/frametime-check.py`
  still meets its budget — a compositor indirection must not regress the
  Issue #14 lag fix.
- **Size.** Large — this is the real architectural cut, expect it to span
  several PRs (own process, IPC/message queue, damage tracking, input
  routing by focus, each separately testable).
- **Model.** [Fable] throughout — this is the highest-risk phase, most of
  `docs/roadmap.md`'s Multi-window section is tagged [Fable] for the same
  reason.
- **Precedent.** Every researched OS with real multi-window support
  (Serenity's `WindowServer`, ToaruOS's `Yutani`, Haiku's `app_server`,
  Redox's `Orbital`) put the compositor in exactly one process and made
  every window a client of it, never a peer. All four also report the
  same lesson in their own docs/discussions: damage tracking (repaint
  only what changed) was retrofitted after the first working version
  did full repaints, not designed in from day one — this kernel already
  has partial-repaint discipline in `drivers/window.c` today (the
  "screen band" and `window_fill_rect_phys`), so Phase 5 should carry
  that forward into the compositor rather than reinvent it.

### Phase 6 — the rest of the apps

- **Goal.** Port every remaining app from Phase 1's extracted files to
  ring 3 against Phase 5's window server, one PR per app or small group,
  in roughly this order: stateless/no-VFS apps first (Toroid, Quotes,
  Search), then single-file VFS apps (Notes, Reminders), then
  network-touching apps last (Mail if it grows a real backend, Chat,
  Epiphany, Stocks) since they exercise the syscall surface hardest.
- **Exit criterion per app.** Same shape as Phase 4's: existing
  `feature-drive.py`/`qa-gallery.py` cases pass unmodified, plus a
  ring-3-confirmed check.
- **Size.** Small per app once the pattern is proven; this is where the
  bulk of the PR count lives.
- **Model.** [Haiku] once two or three apps have proven the port pattern
  (mechanical, known-correct shape); [Sonnet] for the first few and for
  any app with real state-machine complexity (Chat, Epiphany); [Fable]
  only if an app's port surfaces a new ABI gap.
- **Precedent.** This is the "long tail" every researched OS went
  through — Haiku's own app catalog and ToaruOS's userspace both grew
  this way, one port at a time, well after the window server itself was
  solid, never as a single flag-day migration. The corresponding failure
  mode to watch for, per Haiku's own community discussion of its early
  years: doing this migration for its own sake, without new capability,
  can stall a project for a long time if isolation is treated as a
  release blocker rather than an ongoing background conversion. This
  blueprint deliberately makes it incremental and keeps 1.x releases
  shipping features in parallel with the app-by-app migration, not
  gated behind it.

## What this blueprint deliberately does not do

- It does not propose a microkernel rewrite. Redox's own docs make the
  microkernel tradeoff explicit (driver crashes are isolated, at a real
  IPC-overhead cost); Joshua Tree's drivers are trusted, in-tree, and
  reviewed the same way the kernel is, so there's no analogous payoff
  today. The hybrid shape Haiku and ToaruOS both settled on — a
  monolithic-ish kernel plus userland servers for the things that
  actually benefit from isolation (the compositor, apps) — is the closer
  match and is what Phases 4-6 build toward.
- It does not add a GUI toolkit (retained widgets, layout, scene graph).
  `docs/roadmap.md` already parks this explicitly; ring-3 apps in Phase 4
  onward keep drawing immediate-mode against the surface syscalls, the
  same way they draw today, just from a different address space.
- It does not change the syscall ABI's v1 numbers or the existing
  `int 0x80` calling convention. Every new syscall is additive.
