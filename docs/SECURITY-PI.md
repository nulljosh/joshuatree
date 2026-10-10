# Security review: the Pi's network edge

A read-only review of what the ARM64 build exposes to the network: the text browser (`arch/arm64/browser.c`), HTTPS (`arch/arm64/tls.c`, `arch/arm64/tls.h`, the roots in `arch/arm64/certs/`, `tools/gen/tls-ta.py`, `tools/gen/tls-roots.py`), the DNS path (`drivers/net.c`, `drivers/http.c`, `arch/arm64/ip.c`) and the Claude prompt (`arch/arm64/ask.c`, `arch/arm64/claude_cfg.sh`, `arch/arm64/wifi_cfg.sh`, `tools/flash-pi.sh`). Nothing was changed in code. Each finding names the file and line, what goes wrong, the fix, and the check that would prove the fix.

Reviewed on 2026-10-08 against the branch head. Line numbers are from that commit.

## What holds up

- Every HTTPS server certificate is checked against the embedded roots and against the host name asked for. `br_ssl_client_reset(&sc, host, 0)` (`arch/arm64/tls.c:97`) hands BearSSL the name, and `br_x509_minimal` rejects a name mismatch with error 56 and an untrusted chain with error 62. `tools/checks/arm64-tls-check.py` proves the trust check with a build that lacks the test certificate.
- Response size is bounded. The raw reply stops at `RAW_MAX` (64 KiB), the readable text at `TEXT_MAX`, links at `LINK_MAX`, the title at 60 bytes, and the request buffer is sized from `URL_MAX`, so a host plus path from a 255-byte URL cannot overrun it.
- Redirects stop after three hops (`arch/arm64/browser.c:72` and `:109`). `tools/checks/arm64-browser-check.py` proves a self-loop stops.
- The HTML walker stays inside its input. Every scan is bounded by `n` or by the terminating zero that `fetch` writes at `raw[got]`; `put` refuses to write past `TEXT_MAX`; a comment with no end runs to the end of input and stops. No overrun was found.
- The relay token is never printed. It lives in one static buffer, is sent in one Authorization header, and is zeroed after the request (`arch/arm64/ask.c:193` to `:201`). The browser's requests carry no token, no cookie, and no session id. A browsed page is never sent to Claude, and the `[[note]]` and `[[led blink]]` actions are only parsed out of relay replies, never out of a page.
- The relay compares the token in constant time and does not log prompts or replies (`tools/claude-relay/relay.py`).
- The Wi-Fi passphrase never reaches the kernel. `arch/arm64/wifi_cfg.sh` derives the PMK on the Mac and only the PMK is embedded, and only when `JT_WIFI_DEV=1`.

## Findings, most severe first

### 1. High: the relay token is embedded in every Pi build made on the dev Mac, not only the dev card

`arch/arm64/claude_cfg.sh:17` reads `~/.claude-relay-token` whenever the file exists. Only the Wi-Fi key is gated by `JT_WIFI_DEV` (`arch/arm64/wifi_cfg.sh:9`). So `make -C arch/arm64 pi` on the Mac that runs the relay produces a `kernel8.img` with the token inside, and the claim in `CLAUDE.md` that release builds carry neither secret is only true for a build made on a machine without that file. `.github/workflows/release.yml` ships only the i386 ISO, so any Pi image a person hands out comes from the Mac.

What goes wrong: an image copied to a friend's card carries a token that lets anyone on the LAN run `claude -p` on the Mac, as the Mac's user, with whatever tools the relay was started with.

Fix: gate the token on the same dev flag as the Wi-Fi key (`[ "${JT_WIFI_DEV:-0}" = 1 ]` around the read in `claude_cfg.sh`), and rename the flag to something like `JT_DEV_CARD` so its meaning covers both secrets. `tools/flash-pi.sh:39` already sets it.

Check: a script that writes a known 24-byte token to a temp file, builds with `CLAUDE_RELAY_TOKEN_FILE` pointing at it and the dev flag unset, and fails if those bytes appear in `kernel8.img`; then builds with the flag set and fails if they do not.

### 2. High: the relay token crosses the Wi-Fi in clear text

`arch/arm64/ask.c:198` posts to the relay with `http_post_timeout`, plain TCP, with the token in the Authorization header. On QEMU this stays on the Mac. On the Pi, with the relay started `--lan`, the token is visible to anyone on the same network and can be replayed. `relay.py` warns about exactly this at start-up.

What goes wrong: one captured packet gives a stranger the same `claude -p` access as finding 1.

Fix: send the question through `https_fetch` from `arch/arm64/tls.c`. The build already accepts an extra trust anchor through `TLS_TA=`, so the relay can use a self-signed certificate baked into the dev card. If a certificate is too much, replace the static bearer with a per-request HMAC over the body and a counter, which a replay cannot reuse.

Check: extend `tools/checks/arm64-tls-check.py` style setup with the relay behind the throwaway certificate, and fail if the string `Bearer` is seen on the plain TCP port or if the UART ever shows `claude: error` for a correct token.

### 3. High: the clock used for certificate validity is set from an unauthenticated HTTP Date header

`arch/arm64/ip.c:92` to `:122` (`net_clock_sync`) looks up `www.google.com`, does a plain `HEAD /` on port 80 and takes the `Date:` header as the board's UTC. `arch/arm64/tls.c:72` then hands that time to `br_x509_minimal_set_time`. There is no battery clock. The only guard is `year < 2024`.

What goes wrong: anyone on the path (a rogue router, a spoofed DNS answer, see finding 5) sets any date they like, so an expired certificate, or a certificate from a key that was retired and leaked, passes. When the sync fails the fallback is the build time, which is safe against rollback but means a kernel built months ago rejects any certificate issued after its build date with error 54 until the clock sync succeeds. That is a reliability gap that pushes people to trust the plain-HTTP clock.

Fix: never accept a Date earlier than `BUILD_UTC`, which removes rollback; and take the Date from an HTTPS reply instead of port 80. The first HTTPS handshake can run with the build time, which validates any chain that was valid at build, and that reply's Date header then becomes the clock for the rest of the session.

Check: a host-side unit over the Date parser (the same pattern as `tools/checks/dns-txid-check.sh`) that feeds a Date before the build time and asserts the clock stays unset, and a Date after it and asserts it is taken.

### 4. Medium: an HTTPS page may redirect the browser to plain HTTP without a word

`arch/arm64/browser.c:104` resolves the `Location` header and `:110` makes it the new URL. `resolve` accepts an absolute `http://` target from an `https://` page. The page then loads in clear text and the Console shows no sign of it. A link in the page (`href="http://..."`) does the same through `open N`.

What goes wrong: a user who typed `browse example.com`, which this browser upgrades to HTTPS (`:205`), can end up reading a page that a router on the path rewrote.

Fix: in `fetch`, when the current URL is HTTPS and `next` starts with `http://`, print `browser: redirect to http refused` and return. For links, keep following them but print the scheme in the link list, or mark plain links with `[3 http]`.

Check: add a `/down` route to `tools/checks/arm64-browser-check.py` that answers 302 to an `http://` URL, and assert the refusal line and that the plain server never sees a GET.

### 5. Medium: DNS answers are easy to forge on the LAN

`drivers/net.c:648` takes the transaction id from `ticks()`, a 100 Hz counter, so it is a small and predictable number. `:576` fixes the source port at 53000. `:665` matches only ports; the answer's source IP is not compared with the server asked, and the question name in the answer is not compared with the one sent.

What goes wrong: a device on the same Wi-Fi can answer any lookup first. For HTTPS the certificate check still protects the page, because the forged host must present a valid certificate for the real name. For plain HTTP pages and for the clock sync (finding 3) the forger controls the content.

Fix: take the id from `tls.c`'s `entropy` mixer (or the hardware RNG once it is wired), pick a source port from the same source per query, and drop answers whose source IP is not the server asked. Comparing the question section is a few more lines.

Check: extend `tools/checks/dns-txid-check.sh` with a correct-id answer from the wrong source IP and assert it is rejected.

### 6. Medium: a crafted link can split the request line

`parse_url` (`arch/arm64/browser.c:32`) accepts any byte in the host and path. In `to_text`, a quoted `href` is read up to the closing quote (`:150`), so it may contain `\r\n`. `fetch` copies the path into the request (`:77`) with no filter, so a page can make `open N` send `GET /a\r\nX-Injected: 1\r\n HTTP/1.0`. The injected headers go to the same server the link points at, so the damage is limited to that server, but the behaviour is wrong and could be used to smuggle a request through a proxy.

Fix: in `parse_url`, reject any byte below `!` or above `~` in the host or the path, which also closes typed input with odd bytes.

Check: a page in `tools/checks/arm64-browser-check.py` with such a link, asserting `browser: bad url` on `open` and that the server saw no second request.

### 7. Medium: the trust anchor set is small, hand-refreshed and has no expiry check

`arch/arm64/certs/` holds seven roots chosen in `tools/gen/tls-roots.py`. The earliest to expire is GlobalSign Root CA, in January 2028. There is no revocation at all (no CRL, no OCSP, no CT), which is normal for a BearSSL minimal client but should be written down. Nothing in the build or CI fails when a root is near its end, and `tls-roots.py` copies whatever the Mac's store holds, so a bad root on the Mac becomes a bad root on every Pi.

Fix: a check that reads each PEM's `notAfter` and fails when any root expires within a year, run from `tools/checks/ci-suite.sh`.

Check: that script is the check. It should also fail when `TAS_NUM` in the generated header differs from the number of PEM files.

### 8. Low: the TLS random seed comes from the timer

`arch/arm64/tls.c:43` to `:53` seeds BearSSL's DRBG from the generic timer read between short spins, and the comment says so. The ClientHello random and the ECDHE private key come from this seed.

What goes wrong: if the seed is more predictable than it looks, a passive listener could recover the ephemeral key and read the session. The suite list (`:62`) also includes plain RSA key exchange and CBC with SHA-1, so a server can choose a suite with no forward secrecy at all.

Fix: read the BCM2711 hardware RNG on the Pi (the comment already names it), and drop the `BR_TLS_RSA_WITH_*` and `*_CBC_SHA` suites unless a needed site still requires them.

Check: a boot test that prints two 32-byte seeds across two boots and fails when they match is the honest minimum; it cannot run under QEMU because the Pi 4 model does not expose that RNG, so it has to run through `tools/flash-pi.sh` on the board.

### 9. Low: the dev card holds the Wi-Fi PMK and the SSID in the image

`arch/arm64/wifi_cfg.sh` embeds the PMK as a 32-byte array. Anyone with the card can read it from `kernel8.img` and join the network. `arch/arm64/wifi.c:464` prints the SSID to the UART log, which is fine, but the log should never gain a line that prints the key. This is accepted for the dev card and needs no code change; it is listed so the split is clear.

Fix: none. Keep the rule that the dev card never leaves the house, and keep `tools/flash-pi.sh` as the only path that sets the dev flag.

Check: the same script as finding 1 covers it: build with the flag unset and fail if the PMK bytes appear in the image.

## The dev card versus a release card

| | Dev card (`tools/flash-pi.sh`) | Release card |
|---|---|---|
| Wi-Fi SSID and PMK | Embedded, from `~/.config/joshuatree/wifi.conf` | Absent: the Pi only scans |
| Relay host, port, token | Embedded, from `~/.claude-relay-token` | Absent only if the build machine lacks that file (finding 1) |
| Trust anchors | The seven roots, plus any `TLS_TA=` extra | The seven roots |
| Clock | Plain-HTTP Date header, else build time | Same |

## What was not verified

- No run on the real board. Every claim about the Pi is from reading the code; QEMU does not model the Wi-Fi chip or the hardware RNG.
- BearSSL's minimal X.509 engine is known not to enforce name constraints in intermediate certificates. This review did not test it, and the embedded roots are all public CAs for which it matters little.
- `drivers/net.c`'s TCP (one connection, no retransmission) was not reviewed for its own parsing bugs; only the DNS path and the HTTP header helpers were read.
- `arch/arm64/wifi.c`'s WPA2 handshake was not reviewed.
- Whether `www.google.com` keeps answering plain `HEAD /` on port 80 with a Date header.
- The relay's tool sandbox (`~/pi-files`, read only) was read in its header comment only.

## Relay follow-up, 2026-10-09

The API relay's shared-folder reader had a high-severity check/open race: a local process with write access to `~/pi-files` could replace an approved file with a symlink, letting a model read outside the folder. The reader now pins the folder, opens with `O_NOFOLLOW` and checks the opened descriptor is a regular file. Nonblocking open also prevents a replacement FIFO from hanging the relay. `tools/checks/relay-api-check.py` reproduces the old symlink leak and covers FIFO and deletion races. The API key file is now closed after each read.

The existing release-token guard in `arch/arm64/claude_cfg.sh` already addresses finding 1 for normal release builds: only `JT_WIFI_DEV=1` or an explicitly supplied token file embeds a token. Findings 2 and 3 remain open: Pi requests still send a bearer token over HTTP, and the certificate clock still trusts an unauthenticated Date header. TLS entropy remains timer-based. This follow-up does not certify those paths or the hardware.

## TLS and clock follow-up, 2026-10-09

Pi relay requests now use verified HTTPS with no HTTP fallback. LAN relay startup requires a certificate and key; loopback HTTP remains for i386/QEMU. Clock sync accepts only a verified HTTPS Date, refuses dates before the build or previous accepted time, rejects invalid/duplicate dates and preserves its last value on failure. See `docs/RELAY-TLS.md` for migration and checks.

Finding 3 is reduced, not fully closed: the first handshake still validates at build time. An old certificate valid at build time but expired now can pass that bootstrap check. Removing that window needs persistent trusted time or a fresh signed-time protocol. Timer-based TLS entropy also remains open. No live relay was restarted and no card was flashed by this change.
