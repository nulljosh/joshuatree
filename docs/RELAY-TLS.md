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
Replace `192.168.1.10` with the Mac's current LAN address. The address in the
certificate must match `CLAUDE_RELAY_HOST` exactly.

```sh
mkdir -p ~/.config/joshuatree/relay-tls
chmod 700 ~/.config/joshuatree/relay-tls
cd ~/.config/joshuatree/relay-tls
umask 077
openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj '/CN=Joshua Tree relay CA' -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' -keyout ca.key -out ca.pem
openssl req -new -newkey rsa:2048 -nodes -subj '/CN=192.168.1.10' -keyout server.key -out server.csr
```

Create `server.ext` in that folder:

```text
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:192.168.1.10,IP:192.168.1.10
```

Then sign the server certificate:

```sh
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 30 -sha256 -extfile server.ext -out server.pem
```

The DNS-form entry also covers BearSSL's name comparison for the numeric
host used by the Pi. The IP-form entry lets standard clients verify the
same address. Do not copy either private key onto the card or into git.

## Update the relay and card together

From the repository root, start the reviewed relay with TLS:

```sh
python3 tools/claude-relay/relay.py --lan --token-file ~/.claude-relay-token --tls-cert ~/.config/joshuatree/relay-tls/server.pem --tls-key ~/.config/joshuatree/relay-tls/server.key
```

Keep the existing `--api-key-file` option if your relay uses the Messages
API. `--lan` without both certificate flags is refused. Default loopback
HTTP still works for existing i386 and QEMU checks.

Flash your development card with the same Mac address and the public CA:

```sh
CLAUDE_RELAY_HOST=192.168.1.10 TLS_TA="$HOME/.config/joshuatree/relay-tls/ca.pem" tools/flash-pi.sh "/Volumes/PI"
```

Use your actual development-card path. The script replaces its boot files.
A Pi build always uses TLS for relay requests. For a QEMU TLS build, set
`CLAUDE_RELAY_TLS=1` as well; its default remains loopback HTTP.

If the Mac's address changes, issue a matching server certificate and
rebuild the card. Renew the server certificate before it expires. Never
work around a TLS error by disabling certificate checks.

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

The real Pi, Wi-Fi timing and certificate renewal need a board test before
calling the deployment verified.
