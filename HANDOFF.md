# Phone home screen (roadmap 1.8) -- handoff v4

Branch: `feat/phone-home-screen`, worktree `/tmp/jt-home18`. Still not a PR.
This pass hit the 15-minute cap on step 1 of Joshua's review-fix list.
v3 (below the "Done this pass" section) is still accurate for everything
before this pass.

## Done this pass
1. **Fixed the oversized Calendar tile on the phone grid.** Joshua's
   review of `/tmp/jt-home18-home.png` caught "SEP" spilling past both
   edges of the rounded tile and a huge "28". Root cause:
   `gui_calendar_draw_date`'s `size > 40` branch (kernel/kernel.c,
   ~line 4884) was tuned against the Apps-folder grid tile, never
   against the phone home grid's (also `size > 40`) tile, and the two
   don't share proportions.
   Fix: since `wx_text`'s `mul` is an integer upscale (no fractional
   scale path), ~70% of the old glyph scale comes from dropping one
   font face size on each line while keeping `mul_m`/`mul_d` at 2:
   month face 2->0 (24px->16px, mul2 = 32px physical, ~67% of the old
   48px) and day face 3->1 (28px->20px, mul2 = 40px, ~71% of the old
   56px). New `face_m`/`face_d` locals, `size <= 40` branch unchanged
   (still faces 2/3, mul 1).
   Re-rendered `/tmp/jt-home18-home.png` via `phone-boot-check.py` (it's
   the script that produces that file) and looked at a real 4x crop,
   `/tmp/jt-home18-cal-crop4x.png`: SEP now sits inside the tile with
   clear side margins near the top, 28 is optically centered below it,
   both inside the rounded corners. Looks right.
2. **calicon-check.py**: added `red_outside_rounded_rect()` -- counts
   red month-ink pixels that fall outside the tile's own rounded
   corners (radius = `TILE_W * 0.22`, matching
   `gui_draw_one_icon_on`'s `size * 22/100`), wired into the fail
   checks as `red_out_nov` (need 0). Ran `python3 tools/checks/
   calicon-check.py` standalone: **PASS**, including the new assertion
   (0 red pixels outside the rounded rect). This check only ever boots
   the dock tile (74px, the `size <= 40` branch), not the phone tile
   directly -- the new assertion is a real regression guard on that
   tile's own corners, but the phone tile itself is only visually
   verified via the 4x crop above, not asserted on by a QEMU check.
   Worth a follow-up check if there's ever a phone-specific calicon
   check.
3. Also ran standalone and confirmed passing this pass:
   `make kernel.elf` (clean, same 3 pre-existing warnings),
   `bash tools/checks/godfile-check.sh` (kernel.c at exactly 9877, the
   ceiling itself -- trimmed the new code comment to fit; if kernel.c
   needs to grow again for any reason, that comment is the first place
   to cut further, it's already fairly terse),
   `python3 tools/checks/phone-boot-check.py` (full pass, all PASS
   lines, this is also what re-rendered `/tmp/jt-home18-home.png` and
   `/tmp/jt-home18-app.png` for this pass).

## Not committed yet
Working tree has `kernel/kernel.c` and `tools/checks/calicon-check.py`
modified, uncommitted. Commit these together (they're one fix) before
continuing, with the same attribution already used on this branch:
```
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_012RkrsT6Xj7rjRqcshayERp
```

## Left, not started this pass -- ran out of the 15-minute cap
Everything below is from the task brief, still open:
1. **Rest of the check suite**, one at a time (fixed QMP ports collide --
   confirmed again this pass, check `lsof -nP -iTCP -sTCP:LISTEN | grep
   qemu` first; there was an unrelated qemu on port 4631 this pass, not
   mine, didn't touch it):
   `qa-gallery.py` (need 26/26), `keyboard-only-check.py` (need 25/25),
   `apptop-check.py`, `ring3app-check.py`, `ring3calc-check.py`,
   `ring3toroid-check.py`, `bss-margin-check.py`. None of these run this
   pass -- only `phone-boot-check.py`, `calicon-check.py`,
   `godfile-check.sh`, and `make kernel.elf` ran.
2. **`git fetch && git rebase origin/main`** -- not re-checked this
   pass. Re-check before opening the PR; main was at `d1e2db6` (1.7.13)
   as of v3's last check, before this pass started.
3. **`landing/v86/embed.js` phone tour** -- gated on PR #283
   (`phoneSamanthaIntro`) landing on origin/main. Not re-checked this
   pass, see v3 point 3 for the full context. Re-check with the same
   fetch; if landed, wire the phone tour to the grid+chevron; if not,
   skip per the task brief.
4. **`VERSION` / landing facts** -- v3 left `tools/gen/inject-landing-
   facts.py` and `tools/gen/testing-doc.py` unrun; still unrun this
   pass. The phone_home.h ARCHITECTURE.md row from v3 makes the doc-
   coverage number stale in landing/index.html; regenerate before the
   PR.
5. **`tools/ci-local.sh` under the shared lock** -- not started:
   `until mkdir /tmp/jt-cilocal.lock 2>/dev/null; do sleep 30; done;
   tools/ci-local.sh; rmdir /tmp/jt-cilocal.lock`
6. Open the PR as **non-draft** once everything above is green (squash
   auto-merge per house rule), 4-line body: grid of all 26 apps, apps
   open full screen, a tappable back, Calendar icon fixed/rescaled on
   the big tile. Same commit attribution already used on this branch.

## Exact next command
```
cd /tmp/jt-home18
git add kernel/kernel.c tools/checks/calicon-check.py
git commit -m "phone home: shrink oversized Calendar tile text, add rounded-rect regression check"
python3 tools/checks/qa-gallery.py
python3 tools/checks/keyboard-only-check.py
python3 tools/checks/apptop-check.py
python3 tools/checks/ring3app-check.py
python3 tools/checks/ring3calc-check.py
python3 tools/checks/ring3toroid-check.py
python3 tools/checks/bss-margin-check.py
```
Then, if all green: `git fetch && git log origin/main -5` (recheck
#283 and rebase), `tools/gen/inject-landing-facts.py`,
`tools/gen/testing-doc.py`, commit, the ci-local.sh lock wait, push,
open the non-draft PR.
