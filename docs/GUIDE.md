# Joshua Tree user guide

Joshua Tree is an operating system built from scratch. On a Raspberry Pi 4,
it draws its own desktop, reads a USB keyboard, joins Wi-Fi and lets you ask
Samantha questions in Terminal. This guide explains one part at a time.
Other chapters will cover boot, memory, the screen, apps and Samantha.

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
running. Before flashing, set `CLAUDE_RELAY_HOST` to the Mac's local address;
QEMU's default `10.0.2.2` is an emulator address, not your Mac's Wi-Fi address.
The relay port defaults to 8765. See [the relay setup](CLAUDE-APP.md) for the
shared token and the choice of Claude Code or an API-backed relay.

Type a question in Terminal. `/status` shows the selected model, effort and
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
