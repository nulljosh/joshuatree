# Samantha as an agent

Samantha answers at the Pi's `ask>` row through the relay (`tools/claude-relay/relay.py`). Since the agent loop she can also act on the Pi and see what happened. This page says what she can do and where it stops.

## The loop

1. You type a question. The Pi posts it to the relay with its live status (address, clock, Wi-Fi bars).
2. The answer is printed. If it ends with action lines, the Pi runs them, in order, and prints each outcome.
3. What the actions printed goes back to the relay as the next turn, in the same session ("The Pi ran your actions. Results: ..."). Samantha reads it and answers again, with or without more actions.
4. This repeats until an answer has no actions, or the step limit is reached.

Every step prints `agent: step N` in the Console log, so a photo of the screen or the UART log shows how far it went.

## Actions

Each action is alone on its own line at the end of an answer, inside `[[ ]]`, at most 80 characters.

| Action | What the Pi does |
|---|---|
| `[[note TEXT]]` | Prints TEXT as `pi: TEXT`. |
| `[[say TEXT]]` | Prints TEXT as `say: TEXT`. A caption: the ARM build has no voice yet. |
| `[[led blink]]` | Blinks the green light (on QEMU: `pi: no green light on QEMU`). |
| `[[open APP]]` | Opens an app by name. Only the Calculator opens on ARM so far; other names print `pi: no app named ...`. |
| `[[browse URL]]` | Fetches an `http://` or `https://` page through the Console browser and prints it as text. Any other scheme is unknown. |
| `[[calc EXPR]]` | Works out EXPR with the Calculator's evaluator and prints `calc: EXPR = RESULT`. |
| `[[status]]` | Prints the Pi's address, clock and Wi-Fi bars. |

Nothing in this list can change a file on the Pi or on the Mac. The relay's prompt tells Samantha to ask first and explain if a task would need that. Reading the Pi's own SD card is not an action yet: the ARM build has no file reader for the card.

## Limits

- 4 actions per answer. A fifth stays in the text and does not run.
- 5 steps per question. The fifth ends with `agent: step limit`.
- 80 characters per action; longer ones stay in the text.
- The results sent back are cut at 1200 characters.
- An action the Pi does not know prints `agent: ignored [[...]]` and does nothing.
- Esc, or typing `stop` and Enter, while a question runs ends the loop at the next step with `agent: stopped`. At rest, `stop` says `agent: nothing running`.

## The relay side

In `--api-key-file` mode the relay keeps the last 24 messages of each session, so a result turn lands in the conversation it belongs to. In `claude -p` mode the session resumes as before. The `SAMANTHA` prompt lists the actions, says when each helps (browse for a live page, calc for sums, status when asked about the Pi), and states the ask-before-changing-files rule.

## Keys and release builds

A Pi image carries the relay token only when built with `JT_WIFI_DEV=1`, the same flag that carries the Wi-Fi key (`tools/flash-pi.sh` sets it for the dev card). A release image has no relay config and the Console says `claude: no token`. QEMU builds are unchanged.

## Checks

- `tools/checks/pi-actions-check.py`: the parser, compiled on the host.
- `tools/checks/arm64-agent-check.py`: the loop under QEMU against a fake relay: browse then answer, the two caps, Esc, unknown actions.
- `tools/checks/pi-release-notoken-check.sh`: the release rule.
- `tools/checks/arm64-claude-console-check.py`: the plain question path still works.
