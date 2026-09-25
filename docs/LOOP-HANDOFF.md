# Joshua Tree loop handoff (2026-09-25, evening)

## What the loop is

Continue Joshua Tree work headlessly toward 2.0 with merge-on-green CI standing. Stability, usability and performance rank above feature count. Work on branches, hotswap subagents from roadmap.md, verify headless only, and open a PR; merging happens automatically on green CI per Joshua's standing rule.

## Where things stand

Main is 1.1.5 (Chat reply timeout bounded, clipboard check hardened, filerobust subprocess timeouts). Three subagents landed commits and opened PRs yesterday:

1. **PR #169** (branch `claude/notes-0925`): Text selection in Notes (1.2.0), the first step of the typography pass. Also files docs into docs/roadmap.md.

2. **Chat tightening** (branch `claude/chat-tighten`): Removes debug `Host: localhost` header, strips `>>>` prompt from landing demo, redesigns Chat to show what Samantha can do (handle tool calls, not just answer questions). Also renames Chat to Sam per Joshua's call.

3. **Small polish** (branch `claude/polish-0925`): Calendar icon padding, menu bar weather text color fix, remove uptime from About This Computer.

Joshua's handwritten notes (2026-09-25):
- Stability, usability, performance first, all else follows.
- Chat redesign: expand capabilities, drop debug header, show every tool firing in the landing demo.
- Typography and retina polish: no pixels anywhere. Notes gets first pass.
- Calendar, menu bar, About This Computer quick wins.
- Audio driver (AC97) comes later. Music app depends on it.
- Mod feature: users talk to Samantha to customize the OS, settings persist on disk.

## Next, in order

1. Land the three branches: merge PR #169, PR for chat-tighten, PR for polish-0925. CI is green, auto-merge applies.
2. Run the stability items from docs/roadmap.md ranked top-down: text sharp everywhere (typography pass complete), nothing crashes (error handling audit), icons with taste (already done), accessibility floor (every app keyboard-opens).
3. File the three Notes pages Joshua gave: window edges pixely, Files needs view buttons, improve word processor typography.
4. AC97 sound driver and Music app when roadmap reaches it (v100 section); that is future work.

## Restart prompt

```
/loop Joshua Tree loop toward 2.0: merge PRs on green CI (auto), hotswap subagents (max 4 Haiku/Sonnet, 2 if Fable) from docs/roadmap.md focusing on stability/reliability and Joshua's 2026-09-25 notes, headless only, stop at 90% session or weekly usage with /checkpoint.
```
