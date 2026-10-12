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

Console shows what succeeded and what failed. A failed Wi-Fi join does
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
Fn key for F1.
Some dock tiles are placeholders on the Pi. Calculator, Clock, Terminal
and Console are available; the web demo's larger app collection is separate.

### Read the date and time

The Calendar tile shows a short month name and a large day number. Click
it, or find Calendar in Spotlight, to open the month in its own window.
Left and Right step a month, 1 to 4 pick Day, Week, Month or Year, and
Esc closes it. It cannot save events on the Pi yet.

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

The menu bar shows Wi-Fi progress and signal bars. Open Console for the
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

## Drag a window

Press on a window's title bar between the dots and move the mouse: the Console, Terminal, Browser, Clock and Brick windows follow the pointer and stop at the menu bar, the screen edges and the dock. The Calculator and app windows stay where they open. Released in 3.6.0.

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

## Other ways to run it

- **PC USB stick:** download the latest ISO and `SHA256SUMS` from the [release page](https://github.com/nulljosh/joshuatree/releases/latest), which has the copy steps. BIOS boot is checked in QEMU. UEFI and real PC hardware are not verified.
- **Talk to Samantha on a Mac:** run `make talk`, then hold F2. It needs a mic (see `user/samantha.c`). Voice on the real Pi is not verified: no speaker has been tested there.
- **Samantha on the Pi:** she answers at the console through a relay on your Mac that uses the Claude API. You need your own API key. It stays on the Mac and never goes on the Pi. See [the Pi guide](RASPBERRY-PI.md).
- **Ask Claude Code:** run the relay on your Mac and open Claude from the Launchpad. See [the Claude app](CLAUDE-APP.md).
- **Other make targets:** `make samantha`, `make talk`, `make iso`.
- **Real Pi 4 numbers:** memcpy 1109 MB/s, alloc 30 ns/op, Wi-Fi joined in 6.4 s. The full table is in [BENCHMARKS.md](BENCHMARKS.md).
- **Version 3.0** is the Pi release. It cuts the day the board boots the desktop and you can type and click on it ([the plan](LOOP-HANDOFF.md)).
