# Joshua Tree user guide

Joshua Tree is an operating system built from scratch. On a Raspberry Pi 4,
it draws its own desktop, reads a USB keyboard, joins Wi-Fi and lets you ask
Samantha questions in Terminal. This guide explains one part at a time.
The first chapters cover boot, the desktop, Clock, Terminal and Wi-Fi.
Later chapters will explain memory, the screen and more apps.

## Boot: from the card to the desktop

The Pi starts with its own firmware, a small program supplied by Raspberry
Pi. It reads `config.txt` on the card and loads Joshua Tree's `kernel8.img`
into memory. Raspberry Pi OS is not involved.

Joshua Tree's first instructions choose one CPU core to run the kernel,
prepare its stack and clear the memory used by initially empty variables.
The other cores wait. The kernel then sets up memory, the screen, input
and networking, and draws the desktop. It talks to the hardware itself.

The Pi shows a terracotta splash while it boots: the tree mark in the middle and
a thin line under it that grows as the screen, the card, Wi-Fi, the clock and
Bluetooth finish. When Wi-Fi has joined or given up, the clock has synced or
given up and the radio is up (or has said it will not be), the desktop paints
once: wallpaper, menu bar, dock, no window. Any FAIL or oom line the boot prints
ends the splash at once and paints the desktop, so a failure is never hidden behind
it; a crash draws its red panel over the splash. The
boot log is kept all the same, and a click on a dock tile with no app of its
own (Mail, for one) opens the Console with it. Console shows what succeeded
and what failed. A failed Wi-Fi join does
not mean the entire OS failed: you can still use local apps. A desktop
picture proves the screen and boot path work; it does not prove every
app, key, network request or hardware driver works.

The card holds the boot image. Terminal conversations are kept in memory
and disappear when power is removed. Persistent SD saving and updates
without moving the card back to the Mac are still roadmap work.

| File | What it does |
|---|---|
| `tools/pi-config.txt` | Tells the Pi firmware how to load Joshua Tree. |
| `arch/arm64/start.S` | Runs first and prepares the CPU for the C code. |
| `arch/arm64/linker.ld` | Places the kernel's code, data and stack in memory. |
| `arch/arm64/main.c` | Starts the drivers and draws the desktop. |

## Desktop, Clock and Terminal

This chapter describes the 2.35.0 development build in PR #492. Its new
Clock and Calendar icon have QEMU checks; a real Pi check is still needed.
The card Joshua already booted is 2.34.0 and does not have these changes.

### Open an app with the keyboard

Press F1 to open Spotlight, type an app name, then press Enter. Up and Down
choose another match; Escape closes the search. Cmd+Space opens Spotlight
too, like a Mac; Space on its own does nothing. A Mac keyboard may need its
Fn key for F1. With nothing typed, Spotlight lists every app, Calculator
included; type calc and press Enter to open it.
### Copy, paste and the Cmd keys

Cmd is the Windows or Super key on a PC keyboard; Ctrl does the same for
copy and paste. There is one clipboard for the whole Pi, plain text, 4 KB.
Cmd+A selects everything in a note, Cmd+C copies, Cmd+X cuts and Cmd+V pastes,
in Notes, on the Terminal's input line and in the Browser's address bar. In
the Terminal and the address bar the whole line is what copy and cut take, so
Cmd+A has nothing to select there. A note copy longer than 4 KB keeps the
first 4 KB. A paste takes what fits the field and drops the rest; line breaks
become spaces and pasting never runs a command. Cmd+W closes the front window
and Cmd+Q quits the front app; every window here belongs to one app, so both
do the same as its red dot. Cmd+Space opens Spotlight.

Some dock tiles are placeholders on the Pi. Calculator, Clock, Terminal
and Console are available; the web demo's larger app collection is separate.

### Read the date and time

The Calendar tile shows a short month name and a large day number. Click
it, or find Calendar in Spotlight, to open the month in its own window.
Left and Right step a month, 1 to 4 pick Day, Week, Month or Year, and
Esc closes it. The Calendar chapter below says how events are saved.

To see the time clearly, open Spotlight and search for Clock. It shows the
time, date and Vancouver label. The display refreshes each minute. When
network time is unavailable, it shows `--:--` and `Waiting for network time`.
Escape or the red close button returns to the previous pane. There are no
alarms or timers yet. The timezone is fixed to Vancouver, including its
permanent UTC-7 offset from March 8, 2026.

### Keep two conversations

Press F2 or Ctrl+T for Terminal. The Terminal and Samantha dock tiles also
open it. The prompt is `joshua@tree % `, like a zsh shell. Type a command and
press Enter; a word that is not a command prints `zsh: command not found`.
Ask Samantha with `sam QUESTION`.

Press F3 to switch between two Terminal sessions. Each remembers its typed
line, scrollback and assistant conversation until reboot. Finish or stop
a running request before switching. Browser history is shared between the
two sessions. `/clear` starts the current conversation over.

Console shows boot messages and network errors. It does not take questions.
Escape from an idle Terminal returns to Console. While the assistant is
working, Escape asks it to stop between action steps; it may have to wait
for the current network request to finish.

The Bluetooth mouse reconnects by itself after a reboot, from the pairing saved on the card; type `bt forget` to pair a different one.

### The files behind the desktop

| File | What it does |
|---|---|
| `arch/arm64/main.c` | Draws the desktop, date icon and app search; handles app switching. |
| `arch/arm64/clock_ui.h` | Draws the Clock window and refreshes it each minute. |
| `arch/arm64/clock.c` | Obtains trusted network time. |
| `arch/arm64/ask.c` | Keeps the two assistant conversations and sends questions. |

## Wi-Fi: from switching on to asking a question

The Pi needs a network name and password before it can join your home Wi-Fi.
There is no Wi-Fi settings screen yet. You prepare the development card on
the Mac, then start the Pi with that card.

### Prepare your development card

On the Mac, create `~/.config/joshuatree/wifi.conf` with two lines:

```text
ssid=YOUR_NETWORK_NAME
psk=YOUR_NETWORK_PASSWORD
```

Use your actual network name and password. The name can be up to 32 bytes;
the password must be 8 to 63 characters. Keep this file private with
`chmod 600 ~/.config/joshuatree/wifi.conf`. Do not put it in the repository.

From the repository folder, run `tools/flash-pi.sh` with the mounted card's
path, for example `tools/flash-pi.sh "/Volumes/PI"`. Check that this is your
development card first. The script replaces its boot files and keeps the
previous kernel and settings as `.bak` files. It does not format the card.
It ejects the card when finished. Move it to the Pi and switch on.

The flashing script includes the network name and a key derived from the
password. It also includes the relay token if one has been configured on
the Mac. The password itself is not in the image, but the derived key can
still let someone join your network. Keep the card and its image private.
A normal release build includes neither the Wi-Fi key nor the default
relay token. An explicitly supplied relay token file is a development
override, used by checks too.

### What happens when the Pi starts

The Pi wakes its Wi-Fi chip and loads the chip's firmware, a small program
that runs inside the chip. It scans for networks, finds the configured name
and proves it knows the password through a WPA2 handshake. It then asks
your router for a local address, such as `192.168.1.20`.

The menu bar shows a Wi-Fi fan: flat ink once joined, dimmed grey until then. Open Console for the
`wifi` status lines if joining fails. A network clock is fetched after the
Pi gets an address. That clock comes from a verified HTTPS reply and cannot move behind the
build date or an already accepted time. The initial check still relies on
the build date; [the TLS setup](RELAY-TLS.md) explains that limit.

Without a configured network, the Pi scans but does not join. Without a
successful join, Samantha cannot reach the relay. The desktop still works.

### Ask Samantha or read a web page

Open Terminal from the dock, or press F2 (Ctrl+T also works). Type `help` for local commands.
Type `browse example.com` to read a web page as text. A bare host name uses
HTTPS. This is a text browser: pages that need JavaScript may not work.
Use `links`, `open 1`, `back`, `forward` and `find WORD` to move around.

For Samantha, the Mac must be on the same reachable network with its relay
running. Before flashing, set `CLAUDE_RELAY_HOST` to the Mac's local address
as a first guess; if the router moves the Mac, the Pi finds the relay on the
LAN by itself ([RELAY-TLS.md](RELAY-TLS.md)). The relay port defaults to 8765. See [the relay setup](CLAUDE-APP.md) for the
shared token and the choice of Claude Code or an API-backed relay.

Type `sam QUESTION` in Terminal. `sam` hands the question over and gives the
prompt back at once, so you can keep typing commands while she thinks; her
answer prints as `sam: ...` above the prompt when it lands. One question
flies at a time (`sam: still thinking`), and `browse` waits for her answer.
`/status` shows the selected model, effort and
whether the relay answered last time. `/clear` starts a new conversation.
`/model haiku` selects the smaller model; `/model auto` restores routing.
`/effort low` requests a shorter, cheaper API answer. In Claude Code relay
mode the effort setting is ignored. `/help` lists these controls.

The Pi-to-relay connection requires HTTPS. Follow [the TLS setup](RELAY-TLS.md)
to give the Mac a certificate and include its public CA on the card. An old
HTTP relay will fail until updated. Keep the relay on your trusted network;
do not expose its port to the public internet.

### If something goes wrong

| What you see | What to check |
|---|---|
| Wi-Fi scans but never joins | Check the network name and password on the Mac, then reflash the development card. |
| `claude: no network` | Check Console's Wi-Fi lines and whether the router gave the Pi an address. |
| `claude: no token` | Configure the relay token on the Mac, then rebuild the development card. |
| `claude: error -401` | The Pi's token and the running relay's token do not match. |
| `claude: timeout` | Check that the Mac is awake, the relay is running and the configured address is reachable. |
| A web page fails with a TLS error | The certificate, host name or clock may be wrong. Do not disable certificate checks. |

### The files behind it, in plain words

| File | What it does |
|---|---|
| `tools/flash-pi.sh` | Builds the development image and copies the boot files to your card. |
| `arch/arm64/wifi_cfg.sh` | Turns the private network settings into a key the Pi can use. |
| `tools/wifi-fw.sh` | Fetches the Wi-Fi chip's firmware for the build. |
| `arch/arm64/wifi.c` | Talks to the chip, scans and joins the network. |
| `drivers/net.c` | Handles addresses and moves packets between machines. |
| `arch/arm64/ip.c` | Connects the shared network code to the Pi. |
| `arch/arm64/clock.c` | Sets the clock from verified HTTPS and refuses time rollback. |
| `drivers/http.c` | Sends plain web requests and reads their replies. |
| `arch/arm64/tls.c` | Checks certificates and encrypts HTTPS web requests. |
| `arch/arm64/browser.c` | Turns a web page into readable text and numbered links. |
| `arch/arm64/claude_cfg.sh` | Includes the relay address and token in a development build. |
| `arch/arm64/ask.c` | Sends your question and shows Samantha's answer. |
| `tools/claude-relay/relay.py` | Receives the question on the Mac and asks Claude to answer it. |

For the technical map and checks, see [Architecture](ARCHITECTURE.md),
[Terminal](TERMINAL.md) and [the Pi security review](SECURITY-PI.md).

## Restart or shut down the Pi

The 2.36.0 development build adds keyboard power controls. Press
Type `reboot` or `shutdown` in Terminal, or press Ctrl+Alt+Delete to restart, or Ctrl+Alt+End to shut down. On a Mac keyboard,
Alt is Option and End may require Fn+Right. Terminal asks for confirmation:
Enter proceeds, Escape cancels. Ordinary letters do not confirm. Wait for
Samantha to finish an active request first.

Conversations are still held in memory and will be lost. Shutdown asks the
firmware to power down the SD interface and halt; it does not remove the
board's physical power supply. Restart boots the OS again. These paths
pass in QEMU; confirm them on the board before relying on them. The current
card has not been updated.

## Drag and resize a window

Press on a window's title bar between the dots and move the mouse: every window on the Pi (Console, Terminal, Browser, Clock, Brick, Calculator, Calendar, Notes, Music and Books) follows the pointer and stops at the menu bar, the screen edges and the dock. Drag was released in 3.6.0 for the first five and reaches the rest in 3.12.0.

To resize, press within 12 pixels of a window's right edge, bottom edge or left edge (below the title bar) and move: that edge follows the pointer. The bottom-right corner moves two edges at once. A window will not get smaller than a sensible minimum (320 by 220 for the Console, Terminal, Browser and Clock, 220 by 260 for the Calculator, 400 by 300 for an app), nor bigger than the screen above the dock. What is in the window lays itself out again to fit: the Console and Terminal fit more or fewer columns and rows, the Browser re-wraps its page, the Calculator spreads its keys, and Calendar, Notes, Music and Books redraw at the new size. The pointer stays an arrow. Brick has a fixed field and keeps its first size as its smallest. On a 720p, 768p or 4K screen an app's window also stops growing at about 376,000 pixels of content (it needs that much room in memory). A 1080p Pi never gets there.

## Play Brick

The 2.37.0 development build adds an original brick-breaker game. Open
Spotlight with F1, type Brick and press Enter. Move the paddle with Left
and Right, A and D, or the mouse inside the playfield. Space or a click
launches the ball. Clear all 32 bricks before losing your three lives.

Space or click pauses and resumes. R starts a new game. After a win or
game over, Space starts again. Escape or the red button closes the game;
opening another app closes it too. Spotlight temporarily suspends the
animation while you search. Scores are not saved and the game is silent.
QEMU checks pass; play on the real board is still pending.

### Short messages in the Pi menu bar

A Wi-Fi join and a Brick result appear beside the clock for four seconds. A new message replaces the old one. Messages do not take the keyboard or mouse away from your app. Released in 2.40.0; a board check remains.

## Screen sleep on the Pi

In 2.41.0, five minutes without keyboard or mouse activity blanks the HDMI picture. Press a key or move the mouse to wake it; that input still reaches the app. Brick and an active Samantha request keep the screen awake. Apps, USB and networking keep running; this does not suspend the computer or save your files.

QEMU checks the idle timer, refused requests and both input paths through the firmware mailbox. QEMU acknowledges blanking without hiding its pixels, so HDMI sleep and wake still need a physical Pi test after flashing. The browser demo keeps its existing behavior.

## The apps and tools, one at a time

This part describes what is on main now (3.6.0). Every app and command below passes its checks in QEMU. Only some of it has been seen on the real board: the Pi row of [the architecture picture](../architecture.svg) shows which. Open any app with F1 and its name, as described above. Your files live in the card's `JT/` folder.

### Terminal and the shell

Press F2 or Ctrl+T. The prompt is `joshua@tree % `. Type a command and press Enter. A word that is not a command prints `zsh: command not found: WORD`. Page Up, Page Down, Home and End scroll what has printed.

The commands that work today:

| Command | What it does |
|---|---|
| `help` | Lists the commands. |
| `sam QUESTION` | Asks Samantha. See the Samantha chapter. |
| `ls`, `ls DIR`, `cat NAME` | List the card's files and print the first 4096 bytes of one. |
| `write NAME.EXT TEXT`, `rm NAME`, `mkdir NAME` | Make a text file, remove a file, make a folder. All three work inside `JT/` only. The name needs a dot and keeps to the old 8.3 shape. |
| `browse`, `open`, `back`, `forward`, `reload`, `links`, `more`, `up`, `down`, `top`, `bottom`, `find` | The text browser. See the Browser chapter. |
| `bt` | Bluetooth. See the Bluetooth chapter. |
| `beep`, `vol` | `beep` plays a short tone. `vol 40`, `vol up` and `vol down` set the volume from 0 to 100. |
| `userrun NAME` | Starts a user program by name, for testing. |
| `llm PROMPT` | Asks the tiny model built into the kernel. A normal release has no model inside, so it stays quiet. |
| `reboot`, `shutdown` | Restart or switch off. Typing the word is the confirmation. |

`reboot` and `shutdown` ask the firmware to restart or halt. Shutdown stops the board but does not cut its power. Both are refused while Samantha is answering.

While Samantha is answering, the network commands (`browse`, `open`, `back`, `forward`, `reload`, `llm`, `bt` and `userrun`) wait. You get `zsh: sam is using the network, try again when she answers`. Other commands keep working.

### Browser

Open it from Spotlight. It starts with the address bar ready. Type an address and press Enter. A bare name such as `example.com` uses HTTPS. Page text appears in Tree Sans, and links are drawn in terracotta.

- **Tab, then Enter:** Tab picks the next link and scrolls it into view. Tab again wraps back to the first. Enter follows the picked link. A click on a link works too.
- **Scrolling:** Up and Down move a line. Page Up and Page Down move a page. Home and End jump to the top and bottom. The mouse wheel scrolls three lines at a time.
- **Moving around:** Alt+Left is back, Alt+Right is forward, F5 or Ctrl+R reloads. Ctrl+L, or just typing a letter, starts a new address. The Back, Forward and Reload buttons work with the mouse.
- **Esc:** it leaves the address bar first. Esc again closes the window.

The HTTPS part knows 37 root certificates. A site signed by anyone else fails with a TLS error, and that is on purpose.

What does not work yet: JavaScript, images and styled layout. A page that needs them shows as plain text or not at all. It keeps up to 8 KB of text, 32 links and 8 pages of history. The status line says in plain words when a page did not load.

### Books

Books reads plain text. It lists every `.TXT` file in `JT/BOOKS/`. Up and Down pick a book. Enter opens it. Esc closes the app.

In a book, Right or Space turns a page and Left goes back. Esc returns to the list. The page number is saved next to the book, as `NAME.POS`, so the book opens where you stopped. A book over 3 MB is refused with a message. Drop your own `.TXT` files into `BOOKS/` from the Mac.

### Music

Music plays `.WAV` and `.MP3` files from `JT/` and `JT/MUSIC/`. It shows the title and artist from an MP3's tag, the length, and the cover picture when there is one.

Space plays and pauses. Left and Right seek 5 seconds. Up and Down change the volume. N and P go to the next and previous song. S turns shuffle on, and R cycles repeat through off, all and one. You can also click a song or a button, click or drag the seek bar and the volume bar, and scroll the list with the wheel. Esc closes it.

A song over 3 MB is refused. The sound comes out of the headphone jack, which a real board has not yet confirmed.

### Notes

Notes keeps plain text files in `JT/NOTES/`. The left column holds folders and the right column holds notes. Each note shows its first line.

Up and Down pick. Enter opens a folder or a note. Tab swaps columns. N makes a note. F makes a folder. D then D again deletes the selected note, and any other key cancels. Esc closes the app.

In a note, type as usual. Shift with the arrows selects, Ctrl+A selects all, and copy, cut and paste work. A note saves one or two seconds after you stop typing. Esc saves and goes back to the list. Ctrl+S saves and stays.

### Calendar

The Calendar has four views: press 1 for Day, 2 for Week, 3 for Month and 4 for Year. Left and Right step by the view's own unit. Up and Down step too. T jumps to today. The square bracket keys move the selected day by one.

Enter edits the one event for the selected day. Type and press Enter to keep it. Events are saved as lines in `JT/EVENTS.TXT`, one event per day. Esc closes the app.

### Clock

Open Clock from Spotlight. It shows the time, the date and the Vancouver label, and refreshes each minute. It gets the time from the network, so before Wi-Fi joins it shows `--:--` and `Waiting for network time`. There are no alarms or timers. Esc closes it.

### Samantha

Samantha is the assistant. On the Pi you reach her from Terminal with `sam QUESTION`. The Samantha dock tile opens that same Terminal.

She does not run on the Pi. The Pi sends your question to a small program called the relay, running on your Mac. The relay asks the model and sends the answer back. The link is HTTPS with a pinned certificate and a shared token. If the Mac is off, or Wi-Fi is down, she cannot answer. [The Wi-Fi chapter](#wi-fi-from-switching-on-to-asking-a-question) covers the setup.

`sam` hands the question over and gives the prompt back, so you can keep typing. Her answer prints as `sam: ...` when it lands, followed by one line like `sam: 27.4s (find 0.0, tls 0.8, wait 26.6)` that says where the time went: finding the relay, the secure connection, and waiting for the answer. Plain sums such as `sam 10+10` or `sam "2*(3+4)^2"` are worked out on the Pi at once and never leave it. She asks for one or two sentences unless you ask for detail. The Pi remembers the relay that last answered, in memory and in `JT/RELAY.TXT` on the card (an address, no token), so only the first question after the relay moves has to search the network. If nothing comes back in 30 seconds you get `sam: no answer in 30s (relay at 10.0.0.116)` and the prompt is free again. She can also act: a reply can add a note or blink the green light. A model that runs on the Pi itself is planned. Until then, there is only the kernel's tiny `llm` model, and a release build does not carry one.

### Bluetooth

The Pi has a Bluetooth radio and can pair one Bluetooth mouse. Put the mouse in pairing mode, then:

1. Type `bt scan`. It listens for 5 seconds and lists what it hears, numbered, mice first.
2. Type `bt pair 1`, using the number of your mouse. `bt pair` alone picks the strongest mouse. `bt pair NAME` picks by name.
3. Move the mouse. The arrow follows it and clicks work.

When the pairing finishes, the Pi writes it to `JT/BTPAIR.BIN` on the card. On the next boot it reconnects to that mouse by itself, with no scan. Wake the mouse if it sleeps: the Pi keeps trying (12 seconds at a time, with growing pauses, up to 30 tries) and tries again whenever the mouse drops off, so a sleeping mouse comes back with a click. After the 30 tries, `bt reconnect` starts a new run.

`bt status` (or `bt` alone) shows the radio's state, whether it is searching, connected or idle, the saved mouse with only the last byte of its address, how many tries it made and the last error, and whether a pairing is saved. `bt forget` deletes the saved pairing. Then run `bt scan` and `bt pair` to pair a different mouse. Without an SD card the pairing works until the next reboot. The mouse has only been tested against a fake radio so far.

## Other ways to run it

- **PC USB stick:** download the latest ISO and `SHA256SUMS` from the [release page](https://github.com/nulljosh/joshuatree/releases/latest), which has the copy steps. BIOS boot is checked in QEMU. UEFI and real PC hardware are not verified.
- **Talk to Samantha on a Mac:** run `make talk`, then hold F2. It needs a mic (see `user/samantha.c`). Voice on the real Pi is not verified: no speaker has been tested there.
- **Samantha on the Pi:** she answers at the console through a relay on your Mac that uses the Claude API. You need your own API key. It stays on the Mac and never goes on the Pi. See [the Pi guide](RASPBERRY-PI.md).
- **Ask Claude Code:** run the relay on your Mac and open Claude from the Launchpad. See [the Claude app](CLAUDE-APP.md).
- **Other make targets:** `make samantha`, `make talk`, `make iso`.
- **Real Pi 4 numbers:** memcpy 1109 MB/s, alloc 30 ns/op, Wi-Fi joined in 6.4 s. The full table is in [BENCHMARKS.md](BENCHMARKS.md).
- **Version 3.0** is the Pi release. It cuts the day the board boots the desktop and you can type and click on it ([the plan](LOOP-HANDOFF.md)).
