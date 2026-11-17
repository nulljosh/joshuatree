#!/usr/bin/env python3
"""The card's keys (13.19.0: arch/arm64/cardkeys.h, ask.c tok_load, tls.c tls_card_anchor, tools/gen/card-keys.py).

  1. Host, under address and undefined-behaviour sanitizers: card-keys.py turns a throwaway RSA CA and a throwaway EC
     CA into RELAYTA.BIN, and cardta_parse reads both back with the DN and key where the generator put them. A file cut
     short, one with a byte too many, a wrong magic, an unknown type, a tiny modulus and an unknown curve are refused.
     cardkey_token keeps a 16 to 63 character token without its line end and refuses short, long and spaced ones.
  2. QEMU virt, built like a release image as far as the relay goes: TLS to the relay, but no token and no private CA
     in the kernel. The relay on the host runs TLS with a certificate from a throwaway CA. With RELAYKEY.TXT and
     RELAYTA.BIN on the card (written by card-keys.py), `bench` turns on and the Mac reads `man bench` back through it,
     which needs both: the token for the relay, the CA for the handshake. Without them, `bench` says there is no token.
Skips (exit 0) without clang, ld.lld, openssl, mtools or qemu-system-aarch64.
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
from sdcard import PART, build_card
if not all(shutil.which(t) for t in ("clang", "ld.lld", "openssl", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, openssl, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
os.chdir(ROOT)
TMP = Path(scratch_dir("cardkeys"))
arch = ROOT / "arch/arm64"
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]), flush=True)
    if not ok: fails.append(name)
def ssl(*a): subprocess.run(["openssl", *a], check=True, capture_output=True)

# throwaway CAs and a relay certificate signed by the RSA one, named jt-relay
ssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=Card Check CA", "-keyout", str(TMP / "ca.key"), "-out", str(TMP / "ca.pem"))
ssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2", "-subj", "/CN=Card Check EC CA",
    "-keyout", str(TMP / "ecca.key"), "-out", str(TMP / "ecca.pem"))
ssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=jt-relay", "-keyout", str(TMP / "relay.key"), "-out", str(TMP / "relay.csr"))
(TMP / "ext").write_text("subjectAltName=DNS:jt-relay\nbasicConstraints=CA:FALSE\n")
ssl("x509", "-req", "-in", str(TMP / "relay.csr"), "-CA", str(TMP / "ca.pem"), "-CAkey", str(TMP / "ca.key"), "-CAcreateserial", "-days", "2",
    "-extfile", str(TMP / "ext"), "-out", str(TMP / "relay.pem"))
TOKEN = "cardkeys-check-token-0123456789"
(TMP / "token").write_text(TOKEN + "\n")
(TMP / "wifi.conf").write_text("ssid=Card Check\npsk=checkcheck1\n")

def gen(out, ca):
    env = dict(os.environ, CLAUDE_RELAY_TOKEN_FILE=str(TMP / "token"), JT_RELAY_CA=str(ca), WIFI_CONF=str(TMP / "wifi.conf"))
    r = subprocess.run([sys.executable, "tools/gen/card-keys.py", str(out)], env=env, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr

# ---- 1. the formats on the host ----
rc, out = gen(TMP / "rsa", TMP / "ca.pem")
check("card-keys.py writes the three files and says so without printing a key", rc == 0 and "relay key, relay CA, Wi-Fi" in out and TOKEN not in out, out)
rc, _ = gen(TMP / "ec", TMP / "ecca.pem")
rsa_bin, ec_bin = (TMP / "rsa/RELAYTA.BIN").read_bytes(), (TMP / "ec/RELAYTA.BIN").read_bytes()
check("the token file is the token on one line", (TMP / "rsa/RELAYKEY.TXT").read_text() == TOKEN + "\n")
w = (TMP / "rsa/WIFI.TXT").read_text()
check("WIFI.TXT is ssid= and pmk= in hex", w.startswith("ssid=" + "Card Check".encode().hex() + "\npmk=") and len(w.split("pmk=")[1].strip()) == 64, w)
harness = TMP / "h.c"
harness.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include "cardkeys.h"
int main(int argc, char **argv) { (void)argc;
    FILE *f = fopen(argv[2], "rb"); unsigned char *b = malloc(1 << 16); size_t n = fread(b, 1, 1 << 16, f); fclose(f);
    unsigned char *e = malloc(n ? n : 1); for (size_t i = 0; i < n; i++) e[i] = b[i];   /* exactly n bytes, so ASan sees a read past it */
    if (argv[1][0] == 't') { char out[64]; unsigned k = cardkey_token((const char *)e, (unsigned)n, out); printf("%u|%s\n", k, k ? out : ""); }
    else { struct cardta t; int ok = cardta_parse(e, (unsigned)n, &t);
        printf("%d %u %u %u %u %u %u %u %u\n", ok, t.type, t.curve, t.dn_at, t.dn_len, t.a_at, t.a_len, t.b_at, t.b_len); }
    free(b); free(e); return 0;
}
''')
exe = TMP / "h"
r = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(arch), str(harness), "-o", str(exe)],
                   capture_output=True, text=True)
if r.returncode: print("FAIL: cardkeys.h does not build on the host\n" + r.stderr[-1500:]); sys.exit(1)
def run(mode, data):
    p = TMP / "in"; p.write_bytes(data)
    o = subprocess.run([str(exe), mode, str(p)], capture_output=True, text=True, timeout=20)
    if o.returncode: print("FAIL: the host harness crashed\n" + o.stderr[-1500:]); sys.exit(1)
    return o.stdout.strip()
def ta(b): return [int(x) for x in run("a", b).split()]
v = ta(rsa_bin)
check("the RSA anchor reads back: type 1, a 256-byte modulus, e of 3 bytes, ending at the file's end", v[0] == 1 and v[1] == 1 and v[6] == 256 and v[8] == 3 and v[7] + v[8] == len(rsa_bin), v)
check("... its DN is the CA's subject (the name is in it)", b"Card Check CA" in rsa_bin[v[3]:v[3] + v[4]])
v = ta(ec_bin)
check("the EC anchor reads back: type 2, secp256r1 (23), a 65-byte point", v[0] == 1 and v[1] == 2 and v[2] == 23 and v[6] == 65 and v[5] + v[6] == len(ec_bin), v)
for name, bad in (("cut short", rsa_bin[:-1]), ("a byte too many", rsa_bin + b"\0"), ("a wrong magic", b"JTTA2" + rsa_bin[5:]),
                  ("an unknown type", rsa_bin[:5] + b"\x03" + rsa_bin[6:]), ("only the header", rsa_bin[:8]), ("empty", b""),
                  ("an unknown curve", ec_bin[:8 + int.from_bytes(ec_bin[6:8], "big")] + b"\x07" + ec_bin[9 + int.from_bytes(ec_bin[6:8], "big"):]),
                  ("a DN that runs past the end", rsa_bin[:6] + b"\xff\xff" + rsa_bin[8:])):
    check("an anchor file %s is refused (under ASan)" % name, ta(bad)[0] == 0, ta(bad))
tiny = b"JTTA1\x01\x00\x02AB\x00\x10" + b"\x01" * 16 + b"\x00\x03\x01\x00\x01"
check("a modulus under 512 bits is refused", ta(tiny)[0] == 0)
check("a token keeps its text and drops the line end", run("t", (TOKEN + "\r\n").encode()) == "%d|%s" % (len(TOKEN), TOKEN))
for name, bad in (("15 characters", b"a" * 15), ("64 characters", b"a" * 64), ("a space inside", b"0123456789 abcdef"), ("a control character", b"0123456789abcdef\x01")):
    check("a token of %s is refused" % name, run("t", bad).startswith("0|"))
print("PASS: the card's key formats on the host", flush=True)

# ---- 2. a release-style build that only the card's keys connect ----
d = TMP / "bench"; shutil.rmtree(d, ignore_errors=True)
port = (lambda s: (s.bind(("127.0.0.1", 0)), s.getsockname()[1], s.close())[1])(socket.socket())
env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
relay = subprocess.Popen([sys.executable, "tools/claude-relay/relay.py", "--port", str(port), "--cwd", str(TMP), "--timeout", "20", "--bench-dir", str(d),
                          "--tls-cert", str(TMP / "relay.pem"), "--tls-key", str(TMP / "relay.key")],
                         env=dict(env, CLAUDE_RELAY_TOKEN=TOKEN), stdout=subprocess.DEVNULL, stderr=open(TMP / "relay.log", "w"))
q = None
try:
    benv = dict(env, CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=str(TMP / "no-such-token"), CLAUDE_RELAY_TLS="1")
    subprocess.run(["make", "-s", "-C", str(arch), "clean"], capture_output=True)
    r = subprocess.run(["make", "-C", str(arch), "TLS_TA=", "kernel8.elf"], env=benv, capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); raise SystemExit("FAIL: arch/arm64 does not build")
    ta_h = (arch / "tls_ta.h").read_text()
    import re
    m = re.search(r"#define TAS_NUM (\d+)", ta_h), re.search(r"#define TAS_PRIVATE (\d+)", ta_h)
    check("the kernel has no private anchor and no token of its own", m[0] and m[1] and m[0].group(1) == m[1].group(1))
    def boot(tag, with_keys):
        global q
        card = str(TMP / (tag + ".img")); build_card(card, False)
        MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        mt = lambda *a: subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
        mt("mmd", "::JT")
        if with_keys:
            for n in ("RELAYKEY.TXT", "RELAYTA.BIN"): mt("mcopy", str(TMP / "rsa" / n), "::JT/" + n)
        log, sock = TMP / (tag + ".uart"), TMP / (tag + ".qmp")
        for f in (log, sock):
            if f.exists(): f.unlink()
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                              "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                              "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                              "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock,
                              "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        uart = lambda: log.read_text(errors="replace") if log.exists() else ""
        def wait(text, secs):
            for _ in range(int(secs * 10)):
                if text in uart(): return True
                time.sleep(0.1)
            return False
        if not wait("M2 input ready", 60) or not wait("net dhcp 10.0.2.15", 30): raise SystemExit("FAIL: no boot or no DHCP: " + uart()[-400:])
        s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
            while True:
                rr = json.loads(f.readline())
                if "return" in rr or "error" in rr: return rr
        cmd("qmp_capabilities")
        def key(k): cmd("send-key", keys=[{"type": "qcode", "data": k}], **{"hold-time": 50}); time.sleep(0.15)
        key("f2"); wait("terminal open", 10); time.sleep(0.5)
        for ch in "bench": key(ch)
        key("ret")
        return uart, wait
    uart, wait = boot("nokeys", False)
    check("with no keys on the card, `bench` says there is no token", wait("bench: this card has no relay token", 15), uart()[-300:])
    q.kill(); q.wait(); q = None
    uart, wait = boot("keys", True)
    check("with the card's keys, `bench` turns on", wait("bench: on", 15), uart()[-300:])
    r = subprocess.run([sys.executable, "tools/pi-bench.py", "--dir", str(d), "--wait", "60", "man", "bench"], capture_output=True, text=True, timeout=90)
    check("the Mac reads `man bench` back over TLS with the card's token and CA", r.returncode == 0 and "Lets the Mac run Terminal commands" in r.stdout, r.stdout + r.stderr)
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
finally:
    if q: q.kill(); q.wait()
    relay.terminate()
    subprocess.run(["make", "-s", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d card key check(s) failed" % len(fails)); sys.exit(1)
print("PASS: the card's keys: the relay token and CA on the card connect a kernel built with neither, and bad key files are refused")
