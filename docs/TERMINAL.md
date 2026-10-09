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

- Click the Terminal tile in the dock, or press F2 (or Ctrl+T). Esc, with no question running, goes back to the Console.
- Any other dock tile brings the Console back. The red dot closes whichever window is open.
- With no screen at all, F2 still moves the keyboard to the Terminal, and the UART shows everything.

The UART gets every line from both windows, unchanged. The QEMU checks read it there.

## Keys

No mouse is needed. The desktop's keys, in `arch/arm64/main.c` (`ui_key`); `tools/checks/arm64-keys-check.py` presses each one:

| Key | Does |
|---|---|
| F1, Cmd+Space, Ctrl+Space, Alt+Space | Toggle Spotlight: press again to close. Type to filter the dock's names (prefix or any part), Up and Down choose, Enter opens, Esc closes |
| A letter or digit | With no pane holding the keys, Spotlight with that character already typed |
| Enter, Space, Tab | With no pane holding the keys and no dock label, Spotlight, empty |
| F2, Ctrl+T | The Terminal, at once, closing Spotlight if open; it has the keyboard |
| Left, Right | With no pane holding the keys, move the dock's label along the tiles |
| Enter | Open the labelled tile |
| Esc | Clear the label; with the Terminal in front, back to the Console. While a question runs it stops the agent instead. With nothing open it does nothing |
| Page Up, Page Down, Home, End | Scroll the window in front |

Mac-style USB keyboards: Cmd is the GUI modifier (0x08 left, 0x80 right in the boot report). A top row in Mac mode sends F1 and F2 as brightness down and up on a second HID interface; `xhci.c` listens there and turns those into F1 and F2. A keyboard that sends a report ID in front of its report (9 bytes) is read past the ID. `tools/checks/arm64-keydbg-check.py` covers all of it over USB.

A dev card (`JT_WIFI_DEV=1`) shows the last raw HID report in hex and the key's name in the menu bar for 3 seconds after each key, for real-board tests. Release builds leave it out.

A release build has no relay token. Claude then answers `claude: no token`, and the other commands still work. Only the dev card that `tools/flash-pi.sh` writes carries a token. The token comes from a file outside the repo and is never printed.

## Code and checks

- `arch/arm64/main.c`: the two windows, F1 and the dock tile.
- `arch/arm64/ask.c`: the prompt and the commands.
- `tools/checks/arm64-claude-console-check.py`: typing at the Console asks nothing, the Terminal answers, and the answer is not in the Console.
- `tools/checks/arm64-mouse-check.py`: the Terminal tile opens the Terminal with its prompt.

Next: panes (several Terminals side by side) and the Claude-model prompt (`Claude Sonnet 5.5 $`).
