#!/usr/bin/env python3
"""age file encryption on the Pi (13.26.0, arch/arm64/age.c), checked against the real `age` tool both ways.

Headless QEMU virt with a FAT32 card made with mtools; the Terminal (F2) types each command over QMP; the replies are read
on the UART and the files off the card afterwards.
  Boot 1, a card holding a key made by the host's age-keygen (JT/AGEKEY.TXT) and files the host's `age` locked:
    - `age pub` prints the same age1... public key age-keygen printed;
    - `age dec` opens a 150 KB file the host locked to that key (three 64 KiB STREAM chunks), byte for byte;
    - `age enc` locks a 150 KB file to the card's own key and to a second key given as age1..., and the host's
      `age -d` opens both with the matching identities;
    - a file locked to someone else's key is refused ("not locked to this card's key"), and so is a copy of the host's
      file with one payload byte flipped ("changed"), with no output written;
    - `age keygen` refuses to replace the key; `age enc AGEKEY.TXT ...` refuses to touch the key file.
  Boot 2, an empty card: `age keygen` writes a key the host's age-keygen -y reads back to the printed public key, and a
    file the Pi then locks to itself opens on the host with that key.
Neither secret key ever reaches the UART. Fails when age.c is reverted: `age` is not a command.
Skips (exit 0) without clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools or age.
Usage: tools/checks/arm64-age-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "mtype", "age", "age-keygen")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools or age not installed"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name)
    if not ok: fails.append(name + (" -- " + str(detail)[-500:] if detail else ""))

tmp = scratch_dir("arm64-age")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def put(card, name, data):
    f = os.path.join(tmp, "put." + name); open(f, "wb").write(data)
    if mt(card, "mcopy", "-o", f, "::JT/" + name).returncode: print("FAIL: mtools could not write " + name); sys.exit(1)
def get(card, name):
    f = os.path.join(tmp, "got." + name)
    if os.path.exists(f): os.remove(f)
    r = mt(card, "mcopy", "::JT/" + name, f)
    return open(f, "rb").read() if r.returncode == 0 and os.path.exists(f) else None
def keygen():
    f = os.path.join(tmp, "k%d.txt" % time.monotonic_ns())
    subprocess.run(["age-keygen", "-o", f], check=True, capture_output=True)
    text = open(f).read()
    return f, text, re.search(r"public key: (age1[0-9a-z]+)", text).group(1)
def lock(data, pub):
    return subprocess.run(["age", "-r", pub], input=data, capture_output=True, check=True).stdout
def unlock(data, keyfile):
    r = subprocess.run(["age", "-d", "-i", keyfile], input=data, capture_output=True)
    return r.stdout if r.returncode == 0 else None

class Session:
    def __init__(self, tag, card):
        self.log, self.sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % self.sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *k): self.cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.08)
    def boot(self):
        if not self.wait_for("M2 input ready", 90) or not self.wait_for("sd fat32 partition at block 2048", 40): return False
        s = socket.socket(socket.AF_UNIX); s.settimeout(60)
        for _ in range(100):
            try: s.connect(self.sock); break
            except OSError: time.sleep(0.1)
        self.f = s.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities"); time.sleep(0.5)
        n = self.uart().count("terminal open"); self.key("f2")
        end = time.time() + 10
        while time.time() < end and self.uart().count("terminal open") < n + 1: time.sleep(0.1)
        time.sleep(0.4)
        return self.uart().count("terminal open") >= n + 1
    def run(self, line, want, secs=60):   # (seen, the UART text the command caused)
        n = len(self.uart())
        for ch in line: self.key({" ": "spc", ".": "dot", "-": "minus"}.get(ch, ch))
        self.key("ret")
        ok = self.wait_for(want, secs, n)
        time.sleep(0.3)
        return ok, self.uart()[n:]
    def close(self):
        self.q.terminate()
        try: self.q.wait(5)
        except subprocess.TimeoutExpired: self.q.kill()

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: make -C arch/arm64 kernel8.elf failed\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

kA, textA, pubA = keygen()          # the card's key, made by the host
kB, textB, pubB = keygen()          # someone else's
secrets = [l for t in (textA, textB) for l in t.splitlines() if l.startswith("AGE-SECRET-KEY-")]
host_msg = bytes((i * 7 + i // 251) & 0xFF for i in range(150000))
note = (b"Joshua Tree, age on the Pi. " * 6000)[:150001]
sealed = lock(host_msg, pubA)
bad = bytearray(sealed); bad[-40] ^= 1

card = os.path.join(tmp, "card1.img"); build_card(card, False)
if mt(card, "mmd", "::JT").returncode: print("FAIL: mtools could not make JT/"); sys.exit(1)
put(card, "AGEKEY.TXT", textA.encode()); put(card, "HOST.AGE", sealed); put(card, "BAD.AGE", bytes(bad))
put(card, "OTHER.AGE", lock(b"not for this card\n", pubB)); put(card, "NOTE.TXT", note)

S = Session("one", card)
try:
    if not S.boot(): raise SystemExit("FAIL: boot 1 did not reach the Terminal: %r" % S.uart()[-600:])
    ok, out = S.run("age pub", "age: your public key is")
    check("age pub prints the key age-keygen made", ok and pubA in out, out)
    ok, out = S.run("age dec host.age host.txt", "age: ")
    check("age dec opens a file the host's age locked (150 KB, three chunks)", "age: opened" in out, out)
    ok, out = S.run("age enc note.txt note.age", "age: ")
    check("age enc locks a file to the card's own key", "age: locked" in out, out)
    ok, out = S.run("age enc note.txt forb.age " + pubB, "age: ")
    check("age enc locks a file to an age1... key given on the line", "age: locked" in out, out)
    ok, out = S.run("age dec other.age other.txt", "age: ")
    check("a file locked to another key is refused", "not locked to this card" in out, out)
    ok, out = S.run("age dec bad.age bad.txt", "age: ")
    check("a file with one payload byte flipped is refused", "not opened, the contents were changed" in out and "age: opened" not in out, out)
    ok, out = S.run("age keygen", "age: ")
    check("age keygen will not replace the card's key", "already has a key" in out, out)
    ok, out = S.run("age enc agekey.txt leak.age " + pubB, "age: ")
    check("age will not lock the key file itself", "is a key or account file" in out and "locked" not in out, out)
    u1 = S.uart()
finally:
    S.close()

check("the opened file is the host's, byte for byte", get(card, "HOST.TXT") == host_msg)
n_age = get(card, "NOTE.AGE")
check("the host's age -d opens what the Pi locked to its own key", n_age is not None and unlock(n_age, kA) == note)
f_age = get(card, "FORB.AGE")
check("... and what it locked to the second key, with that key", f_age is not None and unlock(f_age, kB) == note)
check("... which the card's own key cannot open", f_age is not None and unlock(f_age, kA) is None)
check("nothing was written for the refused files", all(get(card, x) is None for x in ("OTHER.TXT", "BAD.TXT", "LEAK.AGE")))
check("the card's key file is unchanged", (get(card, "AGEKEY.TXT") or b"").decode() == textA)

card2 = os.path.join(tmp, "card2.img"); build_card(card2, False)
if mt(card2, "mmd", "::JT").returncode: print("FAIL: mtools could not make JT/"); sys.exit(1)
put(card2, "HELLO.TXT", b"made on the Pi\n")
S = Session("two", card2)
try:
    if not S.boot(): raise SystemExit("FAIL: boot 2 did not reach the Terminal: %r" % S.uart()[-600:])
    ok, out = S.run("age keygen", "age: ")
    m = re.search(r"your public key is (age1[0-9a-z]+)", out)
    check("age keygen makes a key and prints its public key", "a new key" in out and m, out)
    ok, out = S.run("age enc hello.txt hello.age", "age: ")
    check("... and locks a file to it", "age: locked" in out, out)
    u2 = S.uart()
finally:
    S.close()
made = get(card2, "AGEKEY.TXT")
kf = os.path.join(tmp, "pi-key.txt")
if made: open(kf, "wb").write(made)
y = subprocess.run(["age-keygen", "-y", kf], capture_output=True, text=True) if made else None
check("the host's age-keygen -y reads the Pi's key to the public key it printed", bool(y and m and y.returncode == 0 and y.stdout.strip() == m.group(1)), y and y.stderr)
h = get(card2, "HELLO.AGE")
check("the host opens the Pi's file with the Pi's key", bool(made and h and unlock(h, kf) == b"made on the Pi\n"))
pi_secret = [l for l in (made or b"").decode().splitlines() if l.startswith("AGE-SECRET-KEY-")]
check("no secret key ever reached the UART", not any(s in u for s in secrets + pi_secret for u in (u1, u2)))
check("no KERNEL CRASH", "KERNEL CRASH" not in u1 + u2)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

if fails:
    print("FAIL:"); [print("  - " + f) for f in fails]; sys.exit(1)
print("PASS: age on the Pi reads and writes what the age tool does: keys, X25519 recipients and STREAM chunks both ways; "
      "foreign and tampered files are refused, and the key file is never an input")
