# The Claude app

Claude is an app in the Launchpad. You type a question, Claude Code answers in the window. Ask it about the code in this repo and it reads the files to answer.

## How it works

Joshua Tree has no TLS and no Node. Claude Code needs both. So Claude Code does not run on Joshua Tree. It runs on your Mac, and Joshua Tree talks to it.

The piece in the middle is the relay, `tools/claude-relay/relay.py`. It is one Python file, standard library only. Joshua Tree sends it a plain HTTP POST. The relay runs `claude -p` (headless Claude Code, on your own logged-in plan) and sends the answer back as plain text.

```
Claude app (ring 3) -> kernel, SYS_HTTP_POST -> relay on your Mac -> claude -p -> answer
```

The relay keeps a session id for each conversation, so a follow-up question knows what came before. Type `/new` in the app to start over.

## Run it

Make a token once and keep it private:

```sh
openssl rand -hex 24 > ~/.claude-relay-token && chmod 600 ~/.claude-relay-token
```

Start the relay from the repo root:

```sh
python3 tools/claude-relay/relay.py --token-file ~/.claude-relay-token
```

It listens on `127.0.0.1:8765`. QEMU's network reaches your Mac's loopback at `10.0.2.2`, so boot with a network card:

```sh
make kernel.elf
qemu-system-i386 -kernel kernel.elf -vga std -net nic,model=rtl8139 -net user
```

In Joshua Tree open Settings, then Assistant. Set **Claude relay** to `10.0.2.2` and port `8765`. Set **Claude token** to the text in `~/.claude-relay-token`. Open Claude from the Launchpad and ask.

To try it without typing into Settings, the boot line can carry the three values for one boot: `-append "open=claude claudehost=10.0.2.2 claudeport=8765 claudetoken=$(cat ~/.claude-relay-token)"`. That puts the token in your shell history and process list, so use it for testing only.

## Security

The relay is a command-capable agent behind an HTTP port. Anyone with the token can make Claude read your files and spend your plan. Treat the token like a password.

What keeps that small:

- **Loopback only.** The relay binds `127.0.0.1` unless you pass `--lan`. QEMU does not need the LAN.
- **A token, checked in constant time.** No token, no start: it must be 16 to 63 characters. It comes from a file or an environment variable, never the command line. A wrong or missing token gets 401 and Claude never starts.
- **The app never holds the token.** Settings keeps it and the kernel adds the `Authorization` header itself, only for `/api/claude` on the relay host. The relay host is its own setting, never the Samantha host, so the token cannot leak to the internet by default.
- **Read-only tools.** Claude gets Read, Grep and Glob, scoped to the repo. Everything else is denied, not asked. None of your MCP servers, hooks or plugins load.
- **Limits.** One question at a time. A 4 KB request cap. A hard timeout, default 150 seconds, after which the whole Claude process tree is killed.
- **Quiet logs.** One line per request: method, path, status, sizes, seconds. Never the token, never a prompt.

What it does not protect against:

- **Your repo is readable.** Claude can read every file under the relay's `--cwd`. Keep secrets out of it, or point `--cwd` somewhere smaller.
- **`--lan` is plain HTTP.** On a LAN the token crosses the wire unencrypted. Anyone who sees one request can replay it. Use `--lan` only on a network you trust.
- **The token sits in `SETTINGS.TXT`.** Like the Mail token, it is plain text on the Joshua Tree disk. See `docs/THREAT-MODEL.md`.
- **`--tools` can widen it.** Passing write tools turns it into an agent that changes files. The relay warns you, loudly.

## What it cannot do yet

This is phase 1. Claude Code runs on the host, through the relay. Joshua Tree is the front end. That is all.

- **No tools on Joshua Tree itself.** Claude cannot read or change Joshua Tree's own files. That is phase 2. It needs real TLS on the OS, or a trusted relay protocol where the relay calls back into the machine, with the same care as this one.
- **Not on the Raspberry Pi yet.** It works in QEMU and on any i386 machine with a supported network card. The ARM64 build has no apps yet, but its Console can ask the same relay (below). On a real Pi that waits on Wi-Fi.
- **Not in the browser demo.** The demo on the landing page cannot reach a relay on your network. The app says so in one red line, at once, and never waits.
- **One answer at a time, no streaming.** The answer arrives whole. While it waits, Weather and Stocks refreshes wait too. The desktop keeps running.
- **Up to 8 KB of answer.** Longer answers are cut. Curly quotes and dashes become plain ASCII, because the OS fonts are ASCII.
- **Sessions live in the relay.** Restart the relay and the next question starts a fresh conversation.

## On ARM64: Claude in the Console

The ARM build has no apps yet, so it asks from the Console instead. Type at the `ask>` row and press Enter; the answer prints in the Console. It talks to the same relay with the same token, read at build time from the same token file (or the file named by `CLAUDE_RELAY_TOKEN_FILE`), with the host and port from `CLAUDE_RELAY_HOST` and `CLAUDE_RELAY_PORT`. The token is compiled into the image and kept out of git, so treat a built Pi image like the token itself. It works on QEMU's virt machine today. A real Pi says `claude: no network` until Wi-Fi joins. Details: the Claude section of [ARM64.md](ARM64.md).

## Checks

- `tools/checks/claude-relay-check.py` runs the relay against a stub `claude`: token refused and accepted, session resume, the exact read-only command line, the prompt on stdin, the timeout kill, the size cap, one at a time, and clean logs. It also runs a copy with the token check removed and proves a tokenless request then gets through.
- `tools/checks/arm64-claude-console-check.py` boots the ARM build with the real relay and the stub: a question typed at the Console's `ask>` row gets the answer printed in 53-column lines, a follow-up resumes the session, and a wrong token, no network and no token each print their own line.
- `tools/checks/ring3claude-check.py` boots QEMU with the real relay and the stub: the answer is drawn, the session resumes, a wrong token, a stopped relay and no relay at all each show the red line.
