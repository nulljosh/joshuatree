#!/usr/bin/env python3
"""The dev card's keys as files on the card (13.19.0, arch/arm64/cardkeys.h), so a signed release image can replace the
kernel with `update install` and still join Wi-Fi and reach the Mac's relay.

    tools/gen/card-keys.py OUT_DIR    writes RELAYKEY.TXT, RELAYTA.BIN and WIFI.TXT into OUT_DIR (the card's JT/)

  RELAYKEY.TXT  the relay token from CLAUDE_RELAY_TOKEN_FILE (default ~/.claude-relay-token), one line
  RELAYTA.BIN   the relay CA from JT_RELAY_CA (default ~/.config/joshuatree/relay-tls/ca.pem) as one trust anchor
  WIFI.TXT      ssid= and pmk= in hex from WIFI_CONF (default ~/.config/joshuatree/wifi.conf), as wifi.c's net_load reads
A file whose source is missing is left out, and the line says so. Never prints a key, a token or a passphrase.
Run with --selftest for the format checks.
"""
import hashlib, importlib.util, os, struct, sys
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tlsta", os.path.join(here, "tls-ta.py")); tlsta = importlib.util.module_from_spec(spec); spec.loader.exec_module(tlsta)

def anchor_bin(der):
    dn, key = tlsta.anchor(der)
    if key[0] == "rsa":
        return b"JTTA1\x01" + struct.pack(">H", len(dn)) + dn + struct.pack(">H", len(key[1])) + key[1] + struct.pack(">H", len(key[2])) + key[2]
    return b"JTTA1\x02" + struct.pack(">H", len(dn)) + dn + bytes([key[1]]) + struct.pack(">H", len(key[2])) + key[2]

def token(path):
    t = open(path).read().strip()
    if not 16 <= len(t) <= 63 or any(not "!" <= c <= "~" for c in t): raise SystemExit("card-keys: the relay token must be 16 to 63 printable characters")
    return t

def wifi(path):
    conf = dict(l.split("=", 1) for l in open(path).read().splitlines() if "=" in l)
    ssid, psk = conf.get("ssid", ""), conf.get("psk", "")
    if not ssid or len(ssid) > 32 or not 8 <= len(psk) <= 63: raise SystemExit("card-keys: wifi.conf needs ssid= (up to 32) and psk= (8 to 63)")
    pmk = hashlib.pbkdf2_hmac("sha1", psk.encode(), ssid.encode(), 4096, 32)
    return "ssid=%s\npmk=%s\n" % (ssid.encode().hex(), pmk.hex())

def main(out):
    os.makedirs(out, exist_ok=True)
    def write(name, data, mode="w"):
        tmp = os.path.join(out, name + ".part")
        with open(tmp, mode) as fh: fh.write(data)
        os.replace(tmp, os.path.join(out, name))
    done = []
    tf = os.path.expanduser(os.environ.get("CLAUDE_RELAY_TOKEN_FILE", "~/.claude-relay-token"))
    if os.path.exists(tf): write("RELAYKEY.TXT", token(tf) + "\n"); done.append("relay key")
    ca = os.path.expanduser(os.environ.get("JT_RELAY_CA", "~/.config/joshuatree/relay-tls/ca.pem"))
    if os.path.exists(ca): write("RELAYTA.BIN", anchor_bin(next(tlsta.pems(ca))), "wb"); done.append("relay CA")
    wc = os.path.expanduser(os.environ.get("WIFI_CONF", "~/.config/joshuatree/wifi.conf"))
    if os.path.exists(wc): write("WIFI.TXT", wifi(wc)); done.append("Wi-Fi")
    print("card keys: " + (", ".join(done) if done else "none (no token, CA or wifi.conf here)") + " on the card, so update install keeps them")

def selftest():
    import tempfile
    d = tempfile.mkdtemp()
    open(d + "/t", "w").write("0123456789abcdef0123\n"); assert token(d + "/t") == "0123456789abcdef0123"
    open(d + "/w", "w").write("ssid=Cafe\npsk=password1\n")
    w = wifi(d + "/w"); assert w.startswith("ssid=43616665\npmk=") and len(w.split("pmk=")[1].strip()) == 64
    # the PMK is IEEE 802.11's PBKDF2-SHA1, 4096 rounds: the published test vector for "password" / "IEEE"
    assert hashlib.pbkdf2_hmac("sha1", b"password", b"IEEE", 4096, 32).hex().startswith("f42c6fc52df0ebef9ebb4b90b38a5f90")
    print("card-keys selftest ok")

if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]: selftest()
    elif len(sys.argv) == 2: main(sys.argv[1])
    else: sys.exit(__doc__)
