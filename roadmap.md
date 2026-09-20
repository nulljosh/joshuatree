# Joshua Tree roadmap

Freestanding i386 kernel, no libc. This file is the forward plan: what's still open, what's parked, what's next. Everything already shipped lives in `git log`, `git tag -l "jt-v*"`, and the real [GitHub releases](https://github.com/nulljosh/joshuatree/releases) (one per version, with real notes), not here.

**Latest**: Real-Time Memory Monitor.

<!-- NOTE: The **Latest** field is public-facing copy synced to the landing page's h1/eyebrow. Must read as a real feature announcement ("Introducing X."), never a changelog line. Update alongside version bumps. tools/gen/inject-landing-headline.sh reads this line automatically. -->

**Model routing on each open item**: `[Haiku]` -- mechanical work with a known-correct shape, cheap and low-risk. `[Sonnet]` -- the general case, real feature work with a clear existing pattern to follow. `[Fable]` -- anything where a subtly wrong answer looks fine and boots fine: privilege isolation, exact stack/register frame layouts, wire-protocol byte layout, memory-model changes, needs the deeper reasoning pass. `[Joshua]` -- a real design or scope call, not code. Tags are a starting guess, re-tag if an item turns out easier or harder once opened.

## Engraving design system (Sep 2026, from Joshua)
One ink on one paper, tone by hatching, from the Joshua Tree Co. mark. Full rule in `CLAUDE.md`'s Theme bullet.
- [ ] [Fable] Kernel goes 1-bit, sequenced AFTER the lag item above, since it is partly the fix for it. Ordered dither fills (25/50/75%, a 4x4 Bayer table) stand in for every grey and every alpha blend: desktop, title bars, disabled states. Flat fills and no blending means smaller damage rects and no per-pixel AA cost. Prior art: Atkinson's QuickDraw patterns on the 1984 Mac, same problem, same answer.
- [ ] [Sonnet] Window chrome: 1px ink border, hatched title bar, no shadows. The arched badge outline (R120/R80/R40) on About and login only.
- [ ] [Sonnet] 18 dock icons redrawn as 1-bit line glyphs; the simplified tree is the launcher.
- [ ] [Haiku] Boot splash: the badge drawn scanline by scanline.
- [ ] [Haiku] `landing/icon.svg` (plus root `icon.svg` and the app `.icns`, `iconsync-check.sh` ties all three) redrawn as the simplified tree. The favicon already uses `landing/mark.png`.

## Explicitly parked / non-goals
- SMP (multi-core), one CPU is plenty until everything above works
- A real filesystem journal / crash-consistency, FAT read support is enough for v4-v5
- Wi-Fi, wired NIC only, QEMU doesn't emulate Wi-Fi hardware anyway
- A real GUI toolkit (retained widgets, layout engine, a scene graph). v11's desktop is real and mouse-driven, but it's one hand-drawn screen with hardcoded hit-test rectangles, not a toolkit anything else could be built on top of. Build one if a second, different screen ever needs the same primitives enough times to justify it.
- gato (the macOS voice kiosk, `~/Documents/Code/gato`), different project, different repo, on purpose. Voice control of this kernel is a real future idea but nowhere near the front of the queue.

## Later product ideas (real, not scheduled, revisit once v8's browser actually renders to the framebuffer)
- Replace the landing page's recorded boot GIF with the real thing once there's something worth interacting with: an actual in-browser demo of this kernel, not a video of one.
- User accounts on the landing page, each with their own space.
- A downloadable, installable image so someone can put this on real hardware, not just QEMU, and actually boot into it (explicit north star, Joshua's own words). Real hardware brings its own driver-compatibility questions (this rtl8139 driver, the PCI enumeration, the ATA driver) that QEMU's emulated devices don't raise, worth its own pass when it's actually time, not assumed to just work.
- Real support for the entire codebase's fleet of apps, not just the handful hand-picked so far (explicit north star, Joshua's own words). v9/v11's `gen_app.sh` pipeline only handles the single-static-file case; most of the fleet is React/Vite (needs a bundler this kernel doesn't have) or split across multiple files `serve_app`'s one-shot single-connection server can't serve together. Getting from "5 apps" to "the whole codebase" is a real, larger project on its own, not a rename of the existing pipeline.
- A real text editor app for the GUI (Joshua's own request), with a font/typeface picker. `font.c` currently extracts exactly one embedded bitmap font (the real IBM CP437 dumped from VGA hardware); a picker needs at least a second real font to switch to, not a dropdown with one option, so this waits on that groundwork rather than shipping a fake selector.

## Accounts, passwords, real security (Sep 2026)
Direct request for solid permissions/sandboxing/user accounts. Real, but sequenced: this kernel has no actual privilege boundary yet (no `int 0x80` gate at the time this was written, every task sharing the same address space and ring-0 access to the raw disk), so a login prompt on top of that would store a real password and protect nothing -- worse than no accounts at all. (The `int 0x80` gate has since shipped, v64.)
- [ ] [Joshua] Once that lands: a real threat model first, not a feature list. What is a Joshua Tree "user account" actually protecting against on a single-machine, single-visitor QEMU/browser demo, no network-facing multi-tenant use case exists today. Worth defining before writing a single line of auth code, the same way v31/v32 got the deeper Fable-level pass because a subtly wrong answer there looks fine and boots fine.

## v100 north star: talk to it with a real voice (Sep 2026)
Direct, explicit: loop until this kernel can be talked to with a real microphone, ElevenLabs for the reply. Real shape, not a vague aspiration:
- Real mic input needs the HDA/AC97 + USB audio driver chain below, no shortcut around it.
- Speech-to-text and ElevenLabs TTS are both plain HTTP API calls, the same shape `chat`'s Ollama call already proves out, just new endpoints once audio capture exists to feed them.
- gato already solved "voice loop talking to an LLM" as a separate macOS app, but it gets macOS's own audio stack for free; this kernel needs its own real audio *driver* first. Hardware-then-software, not software-first.
- Realistic order, most-blocking first: HDA/AC97 driver -> USB host controller + USB audio class (for a real Yeti) -> mic capture loop -> STT call -> LLM call (already real) -> ElevenLabs TTS call -> speaker output. Camera/video response is a separate, later branch off the same audio-driver prerequisite.

Not started. The standing long-horizon target the loop keeps working toward, not a single item to check off.

## Later hardware ideas: mic + camera input (Sep 2026)
Real Yeti mic + camera by v100, unless Apple-API-gated. Two real drivers needed, not one: HDA or AC97 for audio, then USB host controller (UHCI/EHCI/xHCI) + USB audio class for a real Yeti. A phone-as-camera path (Continuity Camera style) only works because the *host* OS exposes the mirrored device to apps running on it; this kernel is the QEMU guest, so it would need the same real driver path any other guest OS does. Realistic order: HDA/AC97 first (closest to existing driver shape), USB host + classes after, camera last. Not started, v100+ tier.

## WiFi, real misconception corrected (Sep 2026)
This kernel's networking works through QEMU's emulated rtl8139 (a virtual *wired* NIC); `-net user` NATs that transparently through whatever real connection the host has, WiFi included, with zero WiFi-specific code anywhere. The host Mac's real WiFi chip is never exposed to the guest. Driver-level WiFi only becomes meaningful once this kernel targets real hardware with a real 802.11 card, and even then it's a large project on its own (chipset driver + WPA supplicant). Not started, not scoped for the emulator target at all.

## Multi-window, honestly scoped: what exists, what this pass shipped, what real windows still need (Sep 2026)
5 of 18 dock apps are multi-window capable (Files, Weather, Reminders, Calendar, Mail). What full multi-window still needs, in order:
- [ ] [Fable] **Window objects with backing stores in `drivers/window.c`.** A `struct window { x, y, w, h, title, z, focused, u32 *buf }` list, each window drawn into its OWN heap buffer, never straight into the framebuffer. Size is the real constraint: AA text is drawn at physical resolution (`window_pixel_phys`), so a backing store for today's 804x345 logical content rect at scale 2 is 1608x690x4 = 4.4MB; three windows is 13MB of a 32MB v86 guest, so either a smaller default window, logical-resolution stores with physical-res text re-rendered at compose time, or a per-window scale choice. Design call before code. Prior art with exactly this shape: ToaruOS's compositor (`yutani`, per-window shared-memory buffers composited in z-order), Haiku's `app_server`, and the OSDev wiki's "GUI" page; borrow the buffer-per-window + damage-rectangle model, don't invent one. **Phase 1 partial, v0.73.0 (Sep 2026)**: a real window list exists and two windows (Files, Weather) genuinely open/draw/close independently and simultaneously, see that entry below for the full real evidence -- but it lives in `kernel/kernel.c` next to the app-dispatch table it needs (not `drivers/window.c`, a deliberate deviation from this sketch, see that entry's reasoning), has no per-window backing store yet (both still draw straight into the shared framebuffer through the existing viewport clip, same as the old single-window model), and only 2 of the 18 apps are wired to it. **Phase 2, v0.74.0 (Sep 2026)**: real click-to-focus (clicking a visible background window raises it, `gui_multiwin_hit_test`) and real z-order compositing (`gui_multiwin_draw_all` and the hit test now share one list, so draw order and input agree) both shipped, see that entry below for the full real pixel evidence. The backing-store box above stays unchecked; that's still real, unstarted work for whichever future pass needs true per-window buffers (today's compositing is correct z-order over a shared framebuffer, not yet double-buffered/backing-stored), same for the 2-window cap and converting apps beyond Files/Weather off their blocking loop.
- [ ] [Fable] **A compositor in `gui_run`.** Desktop, then every window's store in z-order, then the pointer, with damage tracking so a keystroke in one window repaints that window's rect only. This kernel has no vsync and no double buffer (the v40/v45.1 tearing and flashing entries are exactly that pain), so the compositor also becomes the place a real back buffer finally lives; v41's `window_open_scaled` mode set is the only real cost model to test it against.
- [ ] [Fable] **Input routing by focus.** Today `kbd_pop`/`mouse_click_edge` are global and pulled by whichever loop is running. Multi-window needs a focused window (topmost under the last click), key events delivered only to it, click events hit-tested against the z-ordered list (chrome first: its X closes THAT window, its title bar drags it, then the content rect in that window's local coordinates). The v62 absolute pointer makes hit-testing trivial; the routing is the work.
- [ ] [Fable] **Apps stop blocking.** This is the big one and the honest reason it's multiple sessions, not one: 18 apps' loops have to become `open()` / `draw(win)` / `on_key(win, k)` / `on_click(win, x, y)` handlers over a per-app state struct so the compositor can interleave them, OR each app runs as its own kernel task (task.c already preempts, and v64 gave tasks a lifecycle) with the current viewport/target/AA-hook state in `window.c` made per-task and saved/restored on every context switch, plus per-task input queues the router fills. The handler conversion is the safer shape (no per-task draw-state hazard, no stack-per-window memory) and it staggers cleanly: the nine read-only `gui_launch_html` viewers plus Weather/Files/About share `gui_wait_close` and convert as one; Reminders/Trash/Settings/Calendar/Mail share the `sleep_ticks` + `get_key_or_click` list shape and convert as a second batch; Notes, Terminal, Keyrate, Chat each have a real bespoke loop and convert last, one at a time, each behind `appclose-check.py` extended to open two windows and close each from its own X.
- [ ] [Haiku] Once that exists, the two currently-unlit traffic lights (`gui_draw_app_titlebar`'s own comment says why they're unlit today: no windowing state to minimize or maximize into) become real, and the chrome's duplicate in-viewport title goes away.

## Session task queue, real current priorities (Sep 2026)
Feeds the landing page's "Where it's going" card automatically (`tools/gen/landing-roadmap.py`), so keep this section's numbered items real and current.
1. [Sonnet] Kernel code cleanup: split `kernel/kernel.c` (~7,800 lines, the real god file in this repo, everything else large is generated data not code) into per-subsystem files. Do this alone, not alongside other kernel.c-heavy work.
2. [Sonnet] A native Word/Pages-style rich document app, distinct from Notes (stays plain text): real paragraph/run formatting in a real file format.
3. [Sonnet] A native syntax-highlighting code editor and a native fetch-and-install tool over this kernel's own HTTP client (the honest buildable version of "a code editor" and "a package manager", neither of which can run here as themselves, see the Priority queue below).

## Real direction, not a roadmap item yet: free OS, monetize custom hardware (Sep 2026)
Direct statement from Joshua: the OS stays free, monetization is custom hardware built to run it, the same model a lot of hobbyist single-board-computer kits use. Honest gap: everything this kernel drives today is QEMU's *emulated* hardware specifically, none of which maps directly to a real board's real chipsets. A custom-hardware direction means that real-hardware port becomes the actual target for future driver work. Not a coding task yet, a real direction for future prioritization calls.

## 1.0.0: the precondition is done, the product isn't yet (Sep 2026)
This file's own bar for 1.0.0 was a stable syscall ABI with a real external caller -- done. v1 shipped frozen with a real ring-3 reference program (`user/hello.c`) compiled against nothing but the ABI (see `docs/SYSCALL-ABI.md`); v2 added real file writes, seek, argv, and a second real program (`user/note.c`). What "1.0.0" still needs before it's an honest product claim, not just a met precondition: more than one program actually using the ABI for something a person would want, a real shell that launches a program by name (today `exec <file>` needs the exact filename), and a `[Joshua]` call on timing/codename. Not a blocker to keep shipping 0.x versions in the meantime.

## Later idea: location-dynamic satellite wallpaper, switchable in Settings (Sep 2026)
Real dependency chain: (1) real geolocation -- shipped, v71 below. (2) A real satellite/map tile source plus an image decoder -- the map half shipped in v75 (OpenTopoMap, PNG), the satellite half shipped in v0.73.1-v0.73.3 (Google/Bing tiles, JPEG, once a JPEG decoder and an NE2000 driver existed). (3) Wallpaper switching in Settings -- a real, easy extension of the existing Settings/`SETTINGS.TXT` persistence, shipped alongside (2). Recorded verbatim, not yet acted on: "eventually we can make the wallpaper epiphany" -- revisit with Joshua directly before acting on it, not understood well enough to scope honestly yet.

## Later idea: a real JavaScript interpreter, honestly scoped (Sep 2026)
Real V8 is not portable here at all (needs a JIT, a GC, pthreads, mmap, a real libc -- everything this kernel's own "no libc, no dependencies" constraint rules out). A small embeddable interpreter (mujs/early-QuickJS shape: tokenizer, tree-walking evaluator, a small object model) is real and buildable on the existing `kheap.c` allocator, but still a genuinely large, multi-version project on its own. Not started, v100+ tier.

## Later idea: Mail multi-account + real SMTP/IMAP, honestly scoped (Sep 2026)
This kernel has zero TLS/crypto anywhere, and every real mail provider requires STARTTLS or implicit TLS (most also OAuth). Multi-account itself is a trivial data-model extension of `mail.h`; plaintext SMTP/IMAP against a server that still accepts it is real and buildable on the existing TCP stack, but no real consumer provider does anymore. A real TLS implementation is the actual blocker (also unlocks HTTPS for `web`/`browse` and OAuth generally). Not started, v100+ tier. Mail stays what it honestly is: a local notes-shaped app with mail chrome.

## Later idea: a native Stocks app + why Epiphany itself can't run here (Sep 2026)
Epiphany's real stack (Supabase auth, Stripe, OAuth) is HTTPS-only end to end; this kernel has zero TLS, so a kernel-native port is a strictly harder version of a problem the fleet's own TUI rollout already declined to solve for the same app. A native Stocks app is a lighter, real, Weather-shaped idea (plain-HTTP quote API, no auth, no portfolio state) *if* a plain-HTTP keyless quote source exists -- checked directly, and it doesn't (Stooq/Yahoo/Finnhub/Google Finance all force HTTPS). Shipped anyway in v76 below as clearly-labeled static demo data.

## Later idea: the rest of the stock-macOS app gap, honestly categorized (Sep 2026)
Buildable now, no new subsystem: Contacts, Calculator, a World Clock (all shipped as v70 below, Contacts+Calculator). Blocked, real reasons: Photos needs an image decoder (blocked until PNG/JPEG landed, v74/v76); Music needs real audio output (the same HDA/AC97 prerequisite the v100 voice north star already needs); Maps/Navigation is double-blocked (no real GPS, same missing image decoder, and most tile servers force HTTPS); Messages/FaceTime need a real messaging backend or camera/mic hardware.

## Open, queued (Sep 2026, refreshed against what actually shipped since)
Several items below this line were resolved later the same session and are marked as such rather than silently dropped, so the history stays honest.
- ~~Apps folder wheel-scroll full-screen redraw~~ -- fixed, v0.79.x's per-row damage tracking pass.
- ~~Mail blank content / Calendar dock icon opening Reminders~~ -- root-caused (v0.76.58): the real bug was the 2-window multi-window cap silently dropping the third dock click instead of falling back to a single window. Mail's own "blank content" report could not be reproduced under any real scenario tried; flagged as possibly stale, not confirmed fixed on its own.
- ~~1.0.0's syscall-ABI precondition~~ -- done. v1 shipped frozen (exit/read/write/open/close/time/getpid/sched_yield) with a real ring-3 reference program (`user/hello.c`) compiled against nothing but the ABI. v2 added real file writes, lseek, argv, and a second real program (`user/note.c`). See `docs/SYSCALL-ABI.md`.
- A real comparative pass against apple.com/os/macos's own page structure/animation, to find what the landing page is still missing. Not attempted, real research task.
- Device-frame chrome for the demo (v0.76.38/40 above): still genuinely undone, needs a redesign that doesn't fight the live 16:9 canvas.
- The real, still-open memory-ceiling question: can `pmm_total_frames()`'s ~15M default be raised safely. Not attempted.

## Priority queue, Sep 20 2026 brain dump (handwritten notes, transcribed and triaged)
Owner's own notebook pages, organized here by real priority tier rather than the order they were written in. "Priority" means: how much it moves the OS toward feeling real and finished, weighed against what this kernel can actually do. Items already shipped tonight are marked, not silently dropped. A few items are real misconceptions given this kernel's actual architecture (no libc, no dynamic linker, no real hardware yet) -- flagged plainly with the honest buildable alternative, not silently reinterpreted.

### P0, real bugs worth a look soon
- [ ] [Haiku] The boot sequence briefly shows "memory management" text before the boot screen proper. Possibly stale/leftover debug text from an earlier pass -- worth a real grep-and-confirm, likely a one-line fix once found.
- [ ] [Haiku] Confirm `write`/save genuinely round-trips in the Files app end to end (real report, no specific repro given -- verify rather than assume broken).
- [ ] [Haiku] Landing page contrast in light mode -- `hero-contrast-check.mjs` already asserts one contrast case; audit whether a real gap remains beyond what it covers.

### P1, real and buildable, high value
- [ ] [Sonnet] Lazy-load the boot loading image itself (separate from the already-fixed demo lazy-load), so the loading image never shows visibly pixelated while scaling in.
- [ ] [Sonnet] A real Activity Monitor-style app: this kernel already has real `ps`/`kill`/`mem` primitives at the shell, a GUI window around them is real, scoped, valuable.
- [ ] [Sonnet] File search (Spotlight-style): real, valuable, a real feature gap against "what makes an OS feel finished." Needs a real index-or-scan design, not a stub.
- [ ] [Sonnet] Security basics: a real lock/screen-lock state (distinct from the existing login-at-boot auth), and a real, confirmed shutdown path (verify `reboot`'s own real behavior first, a shutdown may already effectively exist).
- [ ] [Fable] Accounts: sudo/admin privilege levels. Real accounts already exist (v0.77-v0.82); a second privilege tier is a real, scoped extension, not a rebuild.
- [ ] [Sonnet] Custom hotkey rebinding, a real screensaver, dictation/accessibility basics, clipboard support, screenshot hotkeys. Each individually small and real.
- [ ] [Sonnet] Moveable dock position, menu bar customization, a real network-configuration panel in Settings (the wired NIC's real state -- IP, link -- surfaced in the GUI, not a WiFi panel; see reframe below).
- [ ] [Fable] Window/workspace management, tiling: this is NOT a new item, it is the existing "Multi-window, honestly scoped" work already tracked above (compositor, input routing by focus, apps converted off their blocking loop). Route any new tiling/workspace ask through that existing plan, don't fork a second one.
- [ ] [Sonnet] Kernel code cleanup pass, now that `kernel.c` is confirmed the real god file at ~7,800 lines (everyone else large in the tree is generated data, not code) -- splitting it into real per-subsystem files is genuinely worth doing, carefully, once no other agent has it mid-edit.
- [ ] [Fable] Fonts/typography: real per-report follow-up, text still reads as somewhat pixel-ish despite the real typeface/AA work already shipped (v44+). Worth a dedicated look at the AA quality itself, separate from the app-level typography (family/size/weight) that already exists in the editor.

### P2, real, bigger scope, queued behind P0/P1
- [ ] [Sonnet] A real Word/Pages-style rich document app, distinct from Notes (which stays a plain text editor): real paragraph/run-level formatting stored in a real file format, not just live cursor style toggles. Already agreed tonight as a real, scoped, native project.
- [ ] [Sonnet] A native syntax-highlighting code editor, and a native fetch-and-install tool over this kernel's own HTTP client (the honest, buildable version of "a code editor" and "a package manager" -- see the VS Code/Homebrew reframe below). Already agreed tonight, queued behind the kernel.c cleanup.
- [ ] [Joshua] ISO-based install-to-disk flow with real install-speed benchmarks. Real, but currently this kernel only boots via QEMU `-kernel`/multiboot direct load; a general BIOS-bootable ISO installer is a genuinely large, separate project, most relevant once real-hardware boot (below) is underway.
- [ ] [Sonnet] Native app ports (Epiphany etc.) as real ring-3 programs against the syscall ABI. Directly downstream of tonight's v2 ABI work; genuinely buildable now that argv/file-write exist, still a real per-app scoping exercise each time.
- [ ] [Joshua] Plugins system. Real, but needs a design pass first (what a plugin can touch, how it's loaded) before it's a schedulable task.
- [ ] [Sonnet] tmux-equivalent: real tmux itself needs pty/job-control semantics this kernel doesn't have. The honest buildable version is a native split/multiplexed terminal using the windowing system already being built for tiling, not a port.
- [ ] [Joshua] AI agent settings, customizing, bootstrapping, and agent auth/login. Real direction (ties to the already-shipped Chat/LLM host-config work), but too undefined to schedule yet -- needs a real spec of what "an agent account" means on this kernel before it's buildable.

### Reframed: real hardware/userland limits, not bugs to fix
These read as simple asks but hit real architectural walls this kernel has by design. Noted honestly rather than silently built as something else:
- **WiFi drivers, Bluetooth, mic input, camera/selfies, battery/power/display controls, audio.** All need real hardware this kernel doesn't have a driver for yet (WiFi and Bluetooth need real chipsets; audio needs HDA/AC97; camera needs USB host controller + USB video class). This is exactly the existing "free OS, monetize on custom hardware" direction and the v100 voice north star's own prerequisite chain, not a new ask -- see those sections above. Meaningful once this kernel targets real hardware, not before.
- **Default Homebrew install + dotfiles.** Homebrew is a Ruby package manager built around macOS/Linux binaries, a POSIX shell, dynamic linking -- none of which exists here (flat binaries only, no dynamic linker). The honest equivalent is the native fetch-and-install tool queued above under P2.
- **Gaming support (Steam etc).** Steam needs OpenGL/Vulkan, a huge existing userland, and anti-cheat compatibility most real OSes struggle with. Off the table for a freestanding hobby kernel; not reframed, just out of scope.
- **Fully native drivers for real hardware.** Not a new item -- this is the existing, already-documented "free OS, monetize on custom hardware" direction. Real, large, ongoing, not a single task.
- **"Best of Kali/Ubuntu" apps and features.** Partially already true -- v30 already shipped real "basic hacking tools (Kali/Mr. Robot flavored)." Extend that existing set rather than starting a new "which distro to imitate" track.

### Research, not a build task
Real, good questions, but they belong in a design/reference note, not roadmap.md's execution format: how a kernel differs from the OS/GUI layered on it, what marks a genuinely good OS, what makes Apple's experience read as better than Windows/Linux, UNIX vs Linux certification landscape, ideas from Omarchy's own aesthetic choices, a real comparative pass against apple.com's own macOS marketing page structure (also listed above as a landing-page task). Worth a real write-up session, not a checkbox.

### Needs clarification before scoping
- "Golden gate, new features?" -- unclear reference (a specific competing project? a codename?), needs a real answer from Joshua before this can be triaged at all.
- Proprietary filesystem / server mode / GPU framework -- three real but very different-sized ideas compressed into one note line; needs unpacking into separate, real asks before scoping.

## Landing roadmap summary (Sep 2026)
The "Where it's going" card on the landing page now comes from the open entries in the Session task queue above, not static hand-written copy: a build-time generator (`tools/gen/landing-roadmap.py`) takes up to three open, numbered task titles, skips completed entries, and escapes the result for HTML; a roadmap change now triggers the landing deploy workflow, which regenerates the card before upload. Permanent regression checks (`tools/checks/landing-roadmap-check.py`, plus `--check` on the generator itself) cover completion, new tasks, ordering, escaping, empty queues, and malformed markers. No kernel change.
