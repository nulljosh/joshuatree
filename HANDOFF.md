# Phone home screen (roadmap 1.8) -- handoff v2

Branch: `feat/phone-home-screen`, worktree `/tmp/jt-home18`. Still not a PR.
Continues the first handoff (now superseded); all three of Joshua's
screenshot-review fixes are done and passing.

## Done this pass (all three review fixes)
1. **26 apps, one screen, no scroll.** 4 cols put GUI_APPS_FOLDER's 26
   real apps at 7 rows; row 6 (Activity idx24, Clock idx25) silently
   never drew -- confirmed by cropping `/tmp/jt-home18-home.png` and
   looking (row 5's labels sat right at the bottom, nothing below).
   Switched `kernel/phone_home.h` to `PHONE_HOME_COLS=5` (26/5 = 6 rows),
   cell_h=96, tile=48, y0=56 -- every app's cell now lands with real
   margin above 760px logical, not a boundary case. Verified visually
   (`/tmp/jt-home18-home.png`, all 26 icons retina-crisp, even margins)
   and via a new phone-boot-check.py assertion that computes every
   cell's bottom edge and fails if any exceed 760px.
2. **Tappable way back.** `gui_draw_app_titlebar` now draws a single "<"
   back chevron instead of the desktop's three traffic lights when
   `boot_to_phone` (top-left, logical (18,10); the top 40px strip is
   cleared first, since it used to render on top of stale status-bar
   clock pixels -- caught by looking at the first screenshot). A tap
   there is handled in `gui_app_mouse_tick`'s new phone back-zone check:
   consumes the click edge (app underneath never sees it) and calls the
   new `kbd_inject()` (kernel/irq.c/.h -- pushes a scancode into the same
   ring buffer IRQ1 writes) with the real ESC make code 0x01, so every
   app -- in-kernel or ring-3, gui_wait_close-shaped or not -- closes
   through the *exact* `kbd_pop()==27` path a keyboard's Esc already
   drives. One close path, not a second bolted-on one. Verified: tapping
   the chevron on Calendar returns to the grid (new phone-boot-check.py
   assertion); Esc still works too (Keyrate round trip, unchanged
   assertion, still ring-3).
3. **Weather in the status bar.** `phone_status_bar_draw` was already
   reading `weather_text`, but nothing ever populated it on phone --
   the desktop's own weather_fetch() call lives inside gui_run's dock
   loop, which phone mode never reaches (phone_home_run returns before
   it). Added the same "once, then every 10 minutes" fetch gate
   (weather_tried_once/weather_last_tick, same globals, same
   weather_fetch()) inside phone_home_run's own loop. Still reads blank
   in this NIC-less headless QEMU boot -- expected, same caveat every
   other check in this repo already documents for weather/face fetches.

## godfile ceiling (important, nearly blown)
kernel.c's hand-written-file ceiling is 9877 (tools/checks/godfile-
check.sh's RATCHET entry) -- **never raise it**. My first pass put the
titlebar-chevron and back-zone logic directly in kernel.c and blew past
it to 9907. Fixed by moving both into `kernel/phone_home.h` as
`phone_app_titlebar_draw()` / `phone_back_zone_tick()`, with two
one-line forward declarations near `boot_to_phone`'s own definition
(kernel.c can't see phone_home.h's functions yet at gui_draw_app_
titlebar's/gui_app_mouse_tick's line -- phone_home.h is #included right
before gui_run, much later in the file) and single-line call sites at
each call point. kernel.c now sits at **exactly 9877** -- do not add
anything else to kernel.c itself for this feature; new logic belongs in
phone_home.h/.c only, per the task brief. Verified: `bash tools/checks/
godfile-check.sh` -> OK.

## Verified this pass
- `make kernel.elf` -- clean (same 3 pre-existing unrelated warnings).
- `python3 tools/checks/phone-boot-check.py` -- **all scenarios PASS**,
  including two new assertions (all 26 cells fit on screen; tapping the
  back chevron returns to the grid) and the updated titlebar-content
  check (was hardcoded to the old red-dot pixel, now scans the strip
  since phone draws a chevron instead of dots).
- `bash tools/checks/godfile-check.sh` -- OK.
- `python3 tools/checks/bss-margin-check.py` -- PASS, 37KB free.
- Visually confirmed `/tmp/jt-home18-home.png` (all 26 apps, clean grid,
  even margins, retina-crisp) and `/tmp/jt-home18-app.png` (Calendar
  full screen, back chevron top-left, no more stale-clock-under-title
  artifact after the window_rect clear fix).

## Left (not started this pass -- ran out of the 15-minute cap)
1. **Docs**: `docs/roadmap.md` (check off 1.8), `docs/ARCHITECTURE.md`
   row for `kernel/phone_home.h` (repo rule: every counted source file
   needs one in the same PR -- grep the doc-coverage check for the exact
   row shape another header uses, e.g. chat.h's row, and copy it).
2. **Rest of the check suite**: `qa-gallery` 26/26, `keyboard-only`
   25/25, `apptop-check`, ring3 checks (`ring3app`/`ring3calc`/
   `ring3toroid`), none run this session. Given the 5-column regeoming
   and the new back-chevron tap path, keyboard-only and apptop-check in
   particular are worth running before anything else -- they may assume
   the old 4-col layout or the old dot-based titlebar the same way
   phone-boot-check.py did before this pass's fix.
3. **`tools/ci-local.sh`** under the shared lock (per the task brief):
   `until mkdir /tmp/jt-cilocal.lock 2>/dev/null; do sleep 30; done;
   tools/ci-local.sh; rmdir /tmp/jt-cilocal.lock`. Not started.
4. **`landing/v86/embed.js` phone tour**: still does not have
   `phoneSamanthaIntro()` on `origin/main` as of this pass (checked via
   `git fetch && grep phoneSamanthaIntro landing/v86/embed.js` -- no
   match) -- the concurrent PR the original task brief mentioned hasn't
   landed yet. Rebase on `origin/main` again before touching it; if it
   has landed by then, drive the home grid instead of the old desktop
   dock-click tour, keeping `demochat-check.mjs`'s phone pass green.
   Updated grid tap coordinates for whoever writes that (5 cols now, not
   4): `cell_w=86, cell_h=96, tile=48, y0=56, cols=5` (logical 430x760
   phone), `cx = col*86+43`, `cy_bottom = y0 + row*96 + 48` for
   `row=i/5, col=i%5` over `APPS[]` index `i` (Calendar=2, Keyrate=9).
   The back chevron itself is at logical (18,10)..(38,26) roughly --
   tapping anywhere in the top-left ~60x40px closes the open app.
5. `git fetch && git rebase origin/main` -- last done at the start of
   this pass (branch was already up to date then; re-check, other work
   -- Quotes, Homeqi, a phone Samantha picker fix in kernel/chat.h --
   may have landed since).
6. `VERSION`/`landing/version.txt`/`tools/gen/inject-landing-facts.py`
   were already set to 1.8.0 by the first pass -- re-run inject-landing-
   facts.py once more right before opening the PR in case app count or
   doc coverage changed (phone_home.h's ARCHITECTURE.md row from item 1
   above will change doc coverage %).
7. `tools/gen/testing-doc.py` -- not run yet.
8. Open the PR as **non-draft** once everything above is green (squash
   auto-merge per house rule), 4-line body, same commit attribution
   already used on this branch (Claude Opus 5.5 / this session's
   Claude-Session link).

## Exact next command
```
cd /tmp/jt-home18 && git fetch origin main && git log --oneline origin/main -5
git rebase origin/main   # only if anything new landed
ls tools/checks/ | grep -i "qa-gallery\|keyboard-only\|apptop\|ring3"
```
Then work the "Left" list above in order: docs first (cheap, unblocks
nothing else), then the rest of the check suite, then ci-local.sh under
the lock, then the PR.
