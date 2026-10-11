# Encrypted Pi relay

The Pi's Terminal sends its relay token over HTTPS. It refuses a plain HTTP
relay and never retries with an unencrypted request. The i386 app and default
QEMU Terminal still use HTTP on the Mac's loopback address. The i386 app
cannot connect to a TLS LAN relay; its remote LAN use needs a separate
transport upgrade.

Do not restart your existing relay or reflash a card until the change is
approved and its checks pass. The old development card uses HTTP and will
need updating together with the relay.

## Prepare certificates on the Mac

Use a private certificate authority (CA), then give the Pi only its public
certificate. Keep the CA key and server key on the Mac. The commands below
use OpenSSL and create files under `~/.config/joshuatree/relay-tls`.

The server certificate names `jt-relay`, not an address. The Pi checks that
name and our CA, whatever IP the relay answers from, so the Mac's address no
longer has to match anything. Since 2.46.0 the relay is trusted only through
this CA (`TLS_TA`): a certificate from a public root is refused even if it
names `jt-relay`.

```sh
mkdir -p ~/.config/joshuatree/relay-tls
chmod 700 ~/.config/joshuatree/relay-tls
cd ~/.config/joshuatree/relay-tls
umask 077
openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj '/CN=Joshua Tree relay CA' -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' -keyout ca.key -out ca.pem
openssl req -new -newkey rsa:2048 -nodes -subj '/CN=jt-relay' -keyout server.key -out server.csr
```

Create `server.ext` in that folder:

```text
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:jt-relay
```

Then sign the server certificate:

```sh
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 30 -sha256 -extfile server.ext -out server.pem
```

A certificate made for an IP before 2.46.0 is refused now, even from the
right CA: mint this one before reflashing. Do not copy either private key
onto the card or into git. `tools/checks/arm64-relay-discovery-check.py`
makes its throwaway certificates with these same commands.

## Update the relay and card together

From the repository root, start the reviewed relay with TLS:

```sh
python3 tools/claude-relay/relay.py --lan --turing --token-file ~/.claude-relay-token --tls-cert ~/.config/joshuatree/relay-tls/server.pem --tls-key ~/.config/joshuatree/relay-tls/server.key
```

`--turing` answers as Samantha from Turing's `app/serve.py`, so start that
first. Leave it off to use `claude -p`. `--api-key-file` uses the Messages
API and spends its credit, so it also needs `--allow-api-spend`. `--lan` without both certificate flags is refused. Default loopback
HTTP still works for existing i386 and QEMU checks.

Flash your development card with the public CA. `CLAUDE_RELAY_HOST` is now
only a first guess; the Mac's current address saves one probe:

```sh
CLAUDE_RELAY_HOST=192.168.1.10 TLS_TA="$HOME/.config/joshuatree/relay-tls/ca.pem" tools/flash-pi.sh "/Volumes/PI"
```

Use your actual development-card path. The script replaces its boot files.
A Pi build always uses TLS for relay requests. For a QEMU TLS build, set
`CLAUDE_RELAY_TLS=1` as well; its default remains loopback HTTP.

## When the Mac's address changes

Nothing to do. Before a relay request the Pi tries the relay that answered
last, else `CLAUDE_RELAY_HOST`. If that will not connect or will not prove
itself, it broadcasts one small UDP probe to the relay's port on the LAN.
A relay running with TLS answers on the same port number with that port and
nothing else. The Pi then tries the address the answer came from, and the
Terminal says `claude: relay found at <address>`. If no relay answers it
says `claude: no relay answered on the LAN`.

The answer is not trusted. Anyone on the LAN can send one; all it does is
say where to try. The token goes out only after a handshake that proves
`jt-relay` and our CA, and a failed request is never resent, so Claude never
runs twice. A LAN host that answers faster than the relay can stop the Pi
from reaching it, but it cannot read the token.

Renew the server certificate before it expires. Never work around a TLS
error by disabling certificate checks.

## Where the clock comes from

The Pi checks the time server's certificate at the image's build time,
then takes the Date header from a verified HTTPS reply from
`www.google.com`. It refuses malformed dates, duplicate Date headers,
dates before the build and dates before a time already accepted this boot.
An unsuccessful sync leaves the clock unset or preserves its previous time.
There is no HTTP time fallback.

This is a bootstrap improvement, not a battery clock. Before the first sync,
a certificate valid at build time can still be accepted even if it has
expired since then. A stale image can also reject a newer time-server
certificate and need rebuilding. A persistent trusted clock or signed fresh
time is still needed to remove that bootstrap window.

Pi TLS seeds come from the BCM2711 RNG200 hardware FIFO. A health fault or
one-second timeout wipes the seed and refuses the connection. There is no
timer fallback on the Pi. QEMU virt keeps a timer provider for tests only;
it is not a secure deployment target. `tools/checks/arm64-rng-check.py`
checks the driver with fake registers. Register definitions follow the
[upstream RNG200 register map](https://github.com/torvalds/linux/blob/master/drivers/char/hw_random/iproc-rng200.c); the implementation is independent. Hardware output still needs a board test.

## Checks

`tools/checks/arm64-clock-check.py` exercises the clock with sanitizers and
stubbed HTTPS responses. `tools/checks/claude-relay-check.py` checks TLS
startup, authenticated HTTPS and refusal of plaintext requests.
`tools/checks/arm64-claude-console-check.py --tls` uses the real relay and a
stub Claude through QEMU, including a raw port that must see a TLS handshake
and no bearer. `tools/checks/arm64-tls-check.py` rejects an untrusted chain,
a wrong name, an expired certificate and a certificate not yet valid.
`tools/checks/arm64-relay-discovery-check.py` builds with a dead
`CLAUDE_RELAY_HOST`, finds the relay by its probe, and refuses a certificate
from the right CA that names the old IP instead of `jt-relay`.

The real Pi, Wi-Fi timing, the probe crossing a real router and certificate
renewal need a board test before calling the deployment verified.
