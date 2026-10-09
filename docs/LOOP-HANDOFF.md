# Joshua Tree loop handoff (2026-10-09, morning)

## What the loop is

Finish the landing-page QA and dev-kit signup improvements, then merge PR #488 only after the full local gate and GitHub checks pass. Joshua explicitly approved a safe merge. Do not start the broad Pi roadmap loop from this checkpoint.

## Where things stand

Main is 2.33.2 at dd8dd756. Branch `fix/landing-waitlist-feedback` and draft PR #488 contain the 2.33.3 landing validation and error feedback, one mobile composer after boot, updated Pi copy, redesigned welcome email, and roadmap cleanup. The worktree also holds three mobile check updates: tap the boot poster before expecting the real composer; the typing check no longer taps an obsolete phone home-screen icon.

The original full local run finished with three mobile checks failing because they expected the composer before boot. Every other regression check and all four demo checks passed. All three corrected mobile checks now pass individually. The full local gate must run again before marking the PR ready; skipped draft checks are not a merge gate.

The production signup delivered the original confirmation. The redesigned email was sent once to Joshua from the branch's actual handler using an in-memory waitlist, without changing production signup data. Apple Mail access now works. Delivery, header, button, film link and footer are visually confirmed in Mail. Browser QA covered 19 destinations, the 30-second film, five accordions, gallery, privacy, boot, full screen and Escape; no horizontal overflow at 320, 390, 768 or 1280 pixels with accordions open. Safari, Firefox and a physical phone remain unchecked.

Other work: Minesweeper PR #478 remains draft and is outside this session. Pi keyboard repair PR #487 merged; the development card was flashed and safely ejected by the earlier session, with its real-board test still pending. No card was flashed in this landing session. Benchmarks wait until checks and other host work are quiet.

## Next, in order

1. Commit the three corrected browser checks and checkpoint docs, then run `bash tools/ci-local.sh`, one run at a time and two jobs maximum.
2. Review the full result, push and update PR #488 validation. Mark ready only on local green. Wait for actual GitHub checks, not draft skips.
3. Review current diff and head, then squash merge #488 on green using the verified head. No admin bypass. The normal main workflow deploys the landing and Worker.
4. Verify merge, deployment and live page; update the QA report under the Codex chat's outputs folder. Keep unchecked browsers explicit.
5. Resume any Pi or Minesweeper work only from its own approved scope. Benchmark on a quiet Mac.

## Restart prompt

```text
Continue the Joshua Tree landing QA on fix/landing-waitlist-feedback, draft PR #488. Read docs/LOOP-HANDOFF.md and the current check results first. Run the full local gate before ready, then actual GitHub CI. Joshua has approved merging safely on green. Do not bypass failing or skipped checks. Finish the QA report and verify the automatic deploy. Keep usage and replies lean; do not start unrelated roadmap work or subagents.
```
