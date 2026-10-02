# Joshua Tree loop handoff (2026-10-01, morning)

## What the loop is

Build Joshua Tree to 2.0.0, one small PR at a time. docs/VERSIONS.md is the map: 1.8, then 1.9, then 2.0. Each agent gets one shippable slice, about 10 minutes, headless QEMU only, one VM at a time. Two agents at once if either is Fable or Opus, otherwise three. PRs stay draft until `tools/ci-local.sh` is green, then merge on green without asking. While a big PR waits, hold every other merge (main requires up to date branches), and fold small docs changes into a PR that is already open. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR as the change. Zero open issues, always. At 90% session usage, checkpoint and stop starting new work.

## The 2.0.0 gate

2.0.0 ships only when all of these are true and each has a headless check in ci-suite.sh:

1. 1.8 done: a phone home screen, an app grid, one app full screen at a time, a back button.
2. 1.9 done: touch works, an on-screen keyboard, every app readable at phone size.
3. Every app in the APPS[] table runs as its own ring 3 program. No app code left in kernel.c. Done (26 apps: Samantha was last to ship).
4. A check crashes each app on purpose and proves the desktop is still alive after every one. Done for every ring-3 app.
5. Input goes to the focused window only, not a global key pull. Done for ring-3 window apps (1.9.23, `tools/checks/ring3window-check.py`): Reminders beside Notes, keys reach only the focused one. Weather and Stocks are windows too now (`SYS_REFRESH` 398 replaced their exit-code relaunch), so every RING3_APPS row opens as a compositor window. Fallback replaced (2.0 slice): a full window table or failed window launch on the desktop (dock click, `SYS_LAUNCH_REQUEST`) is now an honest refusal, a "Close a window to open <App>" notice plus the serial marker `winrefuse: <App>` (`tools/checks/dockcap-fallback-check.sh` asserts it). Still open, so the item is not fully closed: the blocking launcher and `gui_poll_event`'s pull survive for the phone home grid, the Apps folder (`gui_apps_launch`) and the text-shell `notes`/`chat`/`usertest`/`notetest` commands, which have no compositor loop to host a window. Deleting them needs those three moved onto windows first. WIP (wx slice 2): Apps folder now closes itself and opens the app through gui_multiwin_open / gui_refuse_open; phone grid uses phone_window_run (full-screen window, chevron/Esc kills the task, gui_multiwin_geom is phone-aware); text-shell notes/chat print a pointer. Blocking ring3app_launch, *_ring3_open, gui_poll_event pull and SYS_WINDOW_POLL non-window path are NOT deleted yet: `testapps` (gui_launch loop), weather/stocks_ring3_run and gui_poll_event users still need them. All 26 apps now open as real compositor windows with private memory and per-window input.

## Where things stand

PR #331 (release/2.0.0) marked ready for ship. GitHub CI red only on 29 stale checks (old in-kernel Samantha, verified fine); fix workflow repointing them to land as 2.0.1 on branches fix/ci-a..d. Joshua will admin-merge #331 himself. VERSION 2.0.0. Static checks pass (check-refs, godfile, testing-doc, suite-coverage, bss margin, versionsync). Full CI: 10 pass, 2 fail (feature-drive, facespeak-demo, both fixed on 2.0.1 branches).

- 26 of 26 apps run as protected ring-3 programs, each its own compositor window with private memory and per-window input.
- Clipboard (syscall 399), resize, growable memory (SYS_BRK) and smooth type in every app.
- Mail compose sends through the Worker (Resend, token and rate limit). Worker secret still pending.
- Weather and Stocks refetch in place (SYS_REFRESH).
- Demo tour covers all 26 apps.
- New 2.0 logo and UI pass done.
- Landing synced.

Also shipping with 2.0: authmail /signup double opt-in with rate limits (deployed). Strata Kit: print-ready CAD, 5-step build sheet, CC BY-NC-SA licence, TRADEMARKS.md (branches feat/strata-simple, feat/legal). Ad v1 on landing (feat/ad-landing). Reception slice 1 (feat/reception). Hardware and ad loops: A+ to B, running two more rounds each.

## Next, in order

After #331 merges:
1. fix/ci-a..d (2.0.1 stale check rewrites).
2. feat/ad-landing (ad deployed).
3. feat/legal (Strata licence and trademarks).
4. feat/strata-simple (hardware print-ready).
5. feat/samantha-live (final Samantha).
6. feat/reception (Reception slice).
Then: 2.1 Music, 2.2 Video, 3.0 on the ASRock J4125B-ITX, Strata Kit at 3.1.

## Restart prompt

```
/loop build Joshua Tree as far as possible until 3.0 hardware (2.0, then 2.x per docs/VERSIONS.md), zero issues and PRs; then stop and refresh the ad and Blender model with Joshua's ideas.
```
