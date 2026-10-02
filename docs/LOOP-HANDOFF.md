# Joshua Tree loop handoff (2026-09-30)

## What the loop is

Build Joshua Tree to 2.0.0, one small PR at a time. docs/VERSIONS.md is the map: 1.8, then 1.9, then 2.0. Each agent gets one shippable slice, about 10 minutes, headless QEMU only, one VM at a time. Two agents at once if either is Fable or Opus, otherwise three. PRs stay draft until `tools/ci-local.sh` is green, then merge on green without asking. While a big PR waits, hold every other merge (main requires up to date branches), and fold small docs changes into a PR that is already open. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR as the change. Zero open issues, always. At 90% session usage, checkpoint and stop starting new work.

## The 2.0.0 gate

2.0.0 ships only when all of these are true and each has a headless check in ci-suite.sh:

1. 1.8 done: a phone home screen, an app grid, one app full screen at a time, a back button.
2. 1.9 done: touch works, an on-screen keyboard, every app readable at phone size.
3. Every app in the APPS[] table runs as its own ring 3 program. No app code left in kernel.c. Done (1.9.26: Samantha was the last).
4. A check crashes each app on purpose and proves the desktop is still alive after every one. Done for every ring-3 app (`tools/checks/ring3crash-all-check.py`, 20 of 20 as of 1.9.22).
5. Input goes to the focused window only, not a global key pull. Done for ring-3 window apps (1.9.23, `tools/checks/ring3window-check.py`): Reminders beside Notes, keys reach only the focused one. Weather and Stocks are windows too now (`SYS_REFRESH` 398 replaced their exit-code relaunch), so every RING3_APPS row opens as a compositor window. Fallback replaced (2.0 slice): a full window table or failed window launch on the desktop (dock click, `SYS_LAUNCH_REQUEST`) is now an honest refusal, a "Close a window to open <App>" notice plus the serial marker `winrefuse: <App>` (`tools/checks/dockcap-fallback-check.sh` asserts it). Still open, so the item is not fully closed: the blocking launcher and `gui_poll_event`'s pull survive for the phone home grid, the Apps folder (`gui_apps_launch`) and the text-shell `notes`/`chat`/`usertest`/`notetest` commands, which have no compositor loop to host a window. Deleting them needs those three moved onto windows first. WIP (wx slice 2): Apps folder now closes itself and opens the app through gui_multiwin_open / gui_refuse_open; phone grid uses phone_window_run (full-screen window, chevron/Esc kills the task, gui_multiwin_geom is phone-aware); text-shell notes/chat print a pointer. Blocking ring3app_launch, *_ring3_open, gui_poll_event pull and SYS_WINDOW_POLL non-window path are NOT deleted yet: `testapps` (gui_launch loop), weather/stocks_ring3_run and gui_poll_event users still need them.

## Where things stand

Checkpoint 2026-10-01. 1.9.26 has 26 of 26 apps in ring 3: Samantha was the last, and the in-kernel chat (`chat.h`, `chat_face.h`) is deleted. The dock, the `chat`/`samantha` shell commands, `boot_to_samantha` and phone mode all open `user/samantha.c`; `phone_home.h` stays as the phone's home screen. Gate item 3 is done. 1.9.23 added the window path (`exec_user_window`, `paging_task_map_private`, `r3wins` in syscall.c, `gui_ring3_windowed` in kernel.c): a ring-3 program as a compositor window with private memory and per-window input. Every ring-3 app opens as a window except Weather and Stocks.

- In ring 3: Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar, Reminders, Curbfind, Calendar, Search, Epiphany, Weather, Burrow, Stocks, Mail, Notes, Terminal, Samantha.
- Still in the kernel: nothing.
- Gate item 4 (crash every app) is done for all ring-3 apps. The check parses `RING3_APPS`, so a new port is covered on its own.
- New syscalls: 386 tasks, 387 http_get (Curbfind, Epiphany), 388 readdir (Search, for Files next).
- The 1.9.14 PR also carries the landing work: chat bar on phones, icon buttons, QA fixes, Tech specs, the footer directory.

Lesson: main requires up-to-date branches, so every merge forces the next PR to re-run CI (about 15 minutes). Batch where possible.

Building on Linux changes every other app's committed `.bin` and `drivers/user_*.h`. Never commit those; `git checkout` them.

QA backlog:

- Samantha's window closed itself 8 to 14 s after an error reply on desktop (unconfirmed).
- Stale cursor glyph in Search and Stocks for a moment after opening.
- Clock is hard to reach in the desktop Apps folder (scroll is flaky).
- Mail "New message" should be an inline sheet on phones, not a new window.
- Text still looks soft on phones (canvas scale 1.25x on DPR 3, kernel glyph AA).
- README shields badge showed "invalid" (GitHub side is fine). If it persists, use a self-hosted endpoint badge.

## Next, in order

1. Stocks to ring 3 (Weather shipped in 1.9.22: the kernel's `weather_fetch` writes `WEATHER.TXT` and the app reads it, R exits 7 so the kernel refetches, no new syscall; Epiphany shipped in 1.9.19: its GP chart now draws previous close to last from `/api/quotes`, since `stx_data` is kernel-only and too big for the 2 KB body). Check `weather_text` in `phone_home.h` and Samantha's weather tool still work. Findings from the 1.9.15 pass (no port landed, the session was cut short):
   - Stocks cannot come through `SYS_HTTP_GET` as it is: `/api/stocks?range=N` is eight rows of up to 64 prices (about 3.2 KB) and the call clamps the body to `JT_HTTP_BODY_MAX` (2048 bytes), so the later symbols are cut. Either the Worker gains a per-symbol path or the app keeps the kernel's `stocks_fetch` as the feed. Epiphany's 40-line `/api/quotes` (about 700 bytes) fits, but its GP chart reads the same `stx_data`.
   - Weather is out, so the multiwindow compositor now has draw/on_key hooks only on Burrow, Mail and Notes. The window checks all use Notes as the second window.
2. (done) Every app is in ring 3.
3. 1.9 phone work: touch everywhere, an on-screen keyboard (only Notes has one), every app readable at phone size.
4. Input by focus, then tag 2.0.0.
5. Tour scenes for Activity and the Apps folder (`landing/v86/embed.js`, `tourappcount-check.mjs`).
6. 2.1 Music, 2.2 Video, 3.0 on the ASRock J4125B-ITX, the Strata Kit at 3.1.

## Restart prompt

```
/loop build Joshua Tree to 2.0.0: docs/LOOP-HANDOFF.md has the gate and the order. One ~10 minute slice per agent, two at once if Fable, headless only, draft until ci-local is green, merge on green, hold merges while a big PR waits, zero issues, docs and landing in the same PR, MONEY.md dated line each pitch. Stop starting work at 90% usage.
```
