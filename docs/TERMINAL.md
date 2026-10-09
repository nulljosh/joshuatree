# Where input lives on the Pi

The ARM desktop has two text windows in the same spot. One is in front at a time.

**Console: logs only.** Boot messages, Wi-Fi and errors. It has no input row, and typing with it in front does nothing (the key echoes go to the UART only). It opens at boot, because on a Pi with no serial cable it is the only debug view. Page Up, Page Down, Home and End scroll it.

**Terminal: the command line.** Its bottom row is the `ask>` prompt. Type, Backspace, Enter. Every command runs here and prints here, above the prompt, in 53-column lines. Page Up, Page Down, Home and End scroll it too.

| Command | What it does |
|---|---|
| `browse URL` | Fetches a page and prints it as text with numbered links |
| `open N` | Follows link N of the last page |
| `llm PROMPT` | Runs the local model, if the image has one |
| anything else | Asks Claude through the relay on the Mac |

## Getting there

- Click the Terminal tile in the dock, or press F1. F1 again goes back to the Console.
- Any other dock tile brings the Console back. The red dot closes whichever window is open.
- With no screen at all, F1 still moves the keyboard to the Terminal, and the UART shows everything.

The UART gets every line from both windows, unchanged. The QEMU checks read it there.

## Keys

A release build has no relay token. Claude then answers `claude: no token`, and the other commands still work. Only the dev card that `tools/flash-pi.sh` writes carries a token. The token comes from a file outside the repo and is never printed.

## Code and checks

- `arch/arm64/main.c`: the two windows, F1 and the dock tile.
- `arch/arm64/ask.c`: the prompt and the commands.
- `tools/checks/arm64-claude-console-check.py`: typing at the Console asks nothing, the Terminal answers, and the answer is not in the Console.
- `tools/checks/arm64-mouse-check.py`: the Terminal tile opens the Terminal with its prompt.

Next: panes (several Terminals side by side) and the Claude-model prompt (`Claude Sonnet 5.5 $`).
