# Where input lives on the Pi

The ARM desktop has a Console and two Terminal sessions in the same spot. One is in front at a time.

**Console: logs only.** Boot messages, Wi-Fi and errors. It has no input row, and typing with it in front does nothing (the key echoes go to the UART only). It opens at boot, because on a Pi with no serial cable it is the only debug view. Page Up, Page Down, Home and End scroll it.

**Terminal: the command line.** Its bottom row is a zsh style prompt, `joshua@tree % `. Type, Backspace, Enter. Every command runs here and prints here, above the prompt, in 53-column lines. Page Up, Page Down, Home and End scroll it too. F3 switches between Terminal 1 and 2. Each keeps its own typed line, scrollback, relay conversation, model and effort. A running request must finish or be stopped before switching. These are two views in one window; side-by-side panes remain future work.

| Command | What it does |
|---|---|
| `browse URL` | Fetches a page and prints it as text with numbered links |
| `open N` | Follows link N of the last page |
| `beep [FREQ MS]` | Plays a tone out of the Pi's 3.5 mm jack |
| `vol [0-100\|up\|down]` | Shows or sets the master volume (default 70); up and down step by 10 |
| `llm PROMPT` | Runs the local model, if the image has one |
| `bt`, `bt scan`, `bt pair` | Bluetooth: where the radio is and its version; a 5 second scan listing what it hears; pairing the mouse from that scan so it drives the pointer ([RASPBERRY-PI.md](RASPBERRY-PI.md#bluetooth)) |
| `ls [DIR]` | Lists the SD card's boot partition (its root, or `ls JT`; 8.3 names and sizes) |
| `cat NAME` | Prints the first 4 KB of a file there as text (`cat JT/NOTE.TXT` one folder down) |
| `write NAME.EXT TEXT` | Makes or replaces `JT/NAME.EXT` holding TEXT; boot files are refused |
| `rm NAME`, `mkdir NAME` | Deletes the file `JT/NAME`, or makes the folder `JT/NAME` |
| `sam QUESTION` | Asks Samantha through the relay on the Mac and returns at once; her answer prints as `sam: ...` when it lands. Only one at a time (`sam: still thinking`). `sam` alone opens the chat view (below). While she thinks, other commands run, except `browse`, `open`, `back`, `forward`, `reload`, `llm`, `bt` and `userrun`, which wait for her |
| `reboot` (or `restart`), `shutdown` (or `poweroff`) | Restarts or powers off the Pi at once. Typing the word is the confirmation; while Samantha is answering it says to wait. The chords Ctrl+Alt+Delete and Ctrl+Alt+End still ask first. |
| anything else | Prints `zsh: command not found: WORD` |

The Samantha dock tile and Spotlight's Samantha entry open this same Terminal on the ARM build; type `sam` there.

### Chat view

`sam` with nothing after it turns the Terminal window into a chat. The first row is the header: `Samantha` on the left and, on the right, the relay she will use (`relay 10.0.0.116`, from the address in memory or the one on the card) or `local only` when none has answered yet. A rule sits under it, then the history of your lines (`you: ...`) and hers (`sam: ...`), wrapped to the Terminal's 53 columns in the same font, and at the bottom the input row `you: `. Enter sends the line through the same path as `sam QUESTION`: a sum answers at once on the Pi, a question goes to the relay while a `thinking...` marker shows under the history, and `/model`, `/effort`, `/status`, `/clear`, `/help` and `stop` work as in the shell. Typing while she thinks still shows on the input row; a second question gets `sam: still thinking`.

| Key | What it does |
|---|---|
| Up, Down | Scroll the history one line |
| Page Up, Page Down, Home, End | Half a page, the oldest line, the newest line |
| Esc, or `/quit` | Back to the `joshua@tree % ` prompt. While she is still thinking it leaves as soon as her answer is in |

A new line snaps the view to the newest. F3 (the other Terminal session) waits until you leave. The power chords and the volume keys work as everywhere; `reboot` typed here is just a question. Each row it draws is also written to the UART as `tui| ...`, which is what `tools/checks/arm64-samtui-check.py` reads. The chat view on the i386 build is unchanged.

## Getting there

- Click the Terminal or Samantha tile in the dock, or press F2 (or Ctrl+T). Esc, with no question running, goes back to the Console.
- Calculator opens its own window; Clock opens through Spotlight. Other placeholder dock apps bring the Console back. The red dot closes the window in front.
- With no screen at all, F2 still moves the keyboard to the Terminal, and the UART shows everything.

The UART gets every line from both windows, unchanged. The QEMU checks read it there.

## Keys

No mouse is needed. The desktop's keys, in `arch/arm64/main.c` (`ui_key`); `tools/checks/arm64-keys-check.py` presses each one:

| Key | Does |
|---|---|
| F1, Cmd+Space | Toggle Spotlight: press again to close. Type to filter app names, including Clock (prefix or any part), Up and Down choose, Enter opens, Esc closes |
| A letter or digit | With no pane holding the keys, Spotlight with that character already typed |
| Enter, Space, Tab | With no pane holding the keys and no dock label, Spotlight, empty |
| F2, Ctrl+T | The Terminal, at once, closing Spotlight if open; it has the keyboard |
| F11, F12 | Volume down and up by 10 (0 to 100), before any pane. The menu bar says "Volume N" and the speaker clicks. The Mac media keys Volume Down, Volume Up and Mute do the same (Mute toggles 0 and the last level) |
| F3 | Switch Terminal session when no request is running |
| Left, Right | With no pane holding the keys, move the dock's label along the tiles |
| Enter | Open the labelled tile |
| Esc | Clear the label; with the Terminal in front, back to the Console. While a question runs it stops the agent instead. With nothing open it does nothing |
| Caps Lock | Letters come out capital until you press it again, and Shift flips them back; digits and punctuation ignore it. It works in the Terminal, the Calculator and Spotlight. No keyboard light yet |
| A key held down | A letter, digit, Space or Backspace types again after 400 ms, then every 80 ms, in whatever got the first press, until you let go or press another key. F-keys, Enter, Esc, arrows and Ctrl, Cmd or Alt chords never repeat. A USB keyboard sends a held key once, so `main.c` does the repeating |
| Page Up, Page Down, Home, End | Scroll the window in front |

Mac-style USB keyboards: Cmd is the GUI modifier (0x08 left, 0x80 right in the boot report). A top row in Mac mode sends F1 and F2 as brightness down and up on a second HID interface; `xhci.c` listens there and turns those into F1 and F2. A keyboard that sends a report ID in front of its report (9 bytes) is read past the ID. `tools/checks/arm64-keydbg-check.py` covers all of it over USB.

A dev card (`JT_WIFI_DEV=1`) shows the last raw HID report in hex and the key's name in the menu bar for 3 seconds after each key, for real-board tests. Release builds leave it out.

A release build has no relay token. Claude then answers `samantha: no token`, and the other commands still work. Only the dev card that `tools/flash-pi.sh` writes carries a token. The token comes from a file outside the repo and is never printed.

## Code and checks

- `arch/arm64/main.c`: the two windows, F1 and the dock tile.
- `arch/arm64/ask.c`: the prompt and the commands.
- `tools/checks/arm64-claude-console-check.py`: typing at the Console asks nothing, the Terminal answers, and the answer is not in the Console.
- `tools/checks/arm64-mouse-check.py`: the Terminal tile opens the Terminal with its prompt.

Next: several Terminals side by side. Browser navigation state is still shared; AI conversation state is separate. Sessions live in memory and reset on reboot.
