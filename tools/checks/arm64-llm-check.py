#!/usr/bin/env python3
"""The local language model on ARM (arch/arm64/llm.c): `llm PROMPT` at the Console's ask> row, and Samantha falling back
to it when the Claude relay cannot be reached. Headless QEMU, keys through QMP, the answer read off the UART.

The checkpoint is tiny and random (dim 64, 2 layers, 4 heads sharing 2 key/value heads, a 377-token vocab, its own classifier, made here
with a fixed seed in llama2.c's model.bin and tokenizer.bin formats), so the text it writes is nonsense on purpose. What
the check proves is that the kernel runs the llama2.c forward pass exactly: this file has its own pure-Python copy of
that pass (float64, no numpy), and the kernel's text and token count must match it. The seed is one where every greedy
pick wins by a clear margin, so float32 against float64 rounding cannot flip a token.

  1. net, relay not listening: `llm once upon a time` prints the reference text and "llm: N tokens, X tok/s";
     a plain question gets "claude: error ..." (the relay is unreachable) and then the local model's answer to it.
  2. no network card: a plain question prints "claude: no network", then the local model's answer. This build pads the
     checkpoint with 64 MiB of zeros, so the image and heap reach past the EL0 arena's usual place (RAM + 64 MiB):
     without main.c moving the arena past the heap, the first allocation there is a translation fault.
  3. the LLM_BREAK build (llm.c without its rotary position step) must NOT match: the check can tell.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-llm-check.py   (from the repo root)
"""
import json, math, os, random, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

DIM, HID, LAYERS, HEADS, KVH, SEQ = 64, 172, 2, 4, 2, 160   # SEQ over 128: llm.c clamps a run to 128 positions
MERGES = ["th", "he", "in", "er", "an", "on", "re", " t", " a", " h", "the", " the", "ll", "lo", "hel", "hell", "hello",
          "ce", "up", "im", "me", " o", " u"]
PROMPT, QUESTION = "once upon a time", "hello there"

def make_model(seed):   # (model.bin, tokenizer.bin, the weights as Python floats rounded through float32)
    rnd = random.Random(seed)
    vocab = ["<unk>", "\n<s>\n", "\n</s>\n"] + ["<0x%02X>" % b for b in range(256)] + [chr(c) for c in range(32, 127)] + MERGES
    scores = [0.0] * (len(vocab) - len(MERGES)) + [-float(i) for i in range(len(MERGES))]
    V, kv = len(vocab), DIM * KVH // HEADS
    f32 = lambda v: struct.unpack("f", struct.pack("f", v))[0]
    def mat(n, s): return [f32(rnd.gauss(0, s)) for _ in range(n)]
    def norm(n): return [f32(1 + rnd.gauss(0, 0.1)) for _ in range(n)]
    w = {"emb": mat(V * DIM, 0.3), "rms_att": norm(LAYERS * DIM), "wq": mat(LAYERS * DIM * DIM, 0.15),
         "wk": mat(LAYERS * DIM * kv, 0.15), "wv": mat(LAYERS * DIM * kv, 0.15), "wo": mat(LAYERS * DIM * DIM, 0.15),
         "rms_ffn": norm(LAYERS * DIM), "w1": mat(LAYERS * DIM * HID, 0.15), "w2": mat(LAYERS * HID * DIM, 0.1),
         "w3": mat(LAYERS * DIM * HID, 0.15), "rms_final": norm(DIM), "wcls": mat(V * DIM, 0.3)}
    order = ["emb", "rms_att", "wq", "wk", "wv", "wo", "rms_ffn", "w1", "w2", "w3", "rms_final"]
    model = struct.pack("7i", DIM, HID, LAYERS, HEADS, KVH, -V, SEQ)   # vocab < 0: its own classifier, after freq_cis
    for k in order: model += struct.pack("%df" % len(w[k]), *w[k])
    model += struct.pack("%df" % (SEQ * DIM // HEADS), *([0.0] * (SEQ * DIM // HEADS)))   # the unused freq_cis tables
    model += struct.pack("%df" % len(w["wcls"]), *w["wcls"])
    tok = struct.pack("i", max(len(t) for t in vocab))
    for t, s in zip(vocab, scores):
        b = t.encode("latin-1"); tok += struct.pack("fi", s, len(b)) + b
    return model, tok, w, vocab, scores

def reference(w, vocab, scores, text):   # llama2.c run.c, greedy: (decoded text, positions run, smallest winning margin)
    V, kv, hs, kv_mul = len(vocab), DIM * KVH // HEADS, DIM // HEADS, HEADS // KVH
    look = {t: i for i, t in enumerate(vocab)}
    toks = [1] + ([look[" "]] if text else []) + [look[ch] if ch in look else ord(ch) + 3 for ch in text]
    while True:
        best = None
        for i in range(len(toks) - 1):
            m = look.get(vocab[toks[i]] + vocab[toks[i + 1]])
            if m is not None and (best is None or scores[m] > best[0]): best = (scores[m], i, m)
        if not best: break
        toks[best[1]:best[1] + 2] = [best[2]]
    def mv(W, off, x, d): n = len(x); return [sum(a * b for a, b in zip(W[off + i * n:off + i * n + n], x)) for i in range(d)]
    def rms(x, W, off):
        s = 1 / math.sqrt(sum(v * v for v in x) / len(x) + 1e-5); return [W[off + j] * s * x[j] for j in range(len(x))]
    kc = [[] for _ in range(LAYERS)]; vc = [[] for _ in range(LAYERS)]
    out, token, pos, margin, steps = "", toks[0], 0, 1e9, min(SEQ, 128)
    while pos < steps:
        x = w["emb"][token * DIM:(token + 1) * DIM]
        for l in range(LAYERS):
            xb = rms(x, w["rms_att"], l * DIM)
            q, k, v = mv(w["wq"], l * DIM * DIM, xb, DIM), mv(w["wk"], l * DIM * kv, xb, kv), mv(w["wv"], l * DIM * kv, xb, kv)
            for i in range(0, DIM, 2):
                ang = pos / 10000 ** ((i % hs) / hs); fr, fi = math.cos(ang), math.sin(ang)
                for vec in ([q, k] if i < kv else [q]):
                    a, b = vec[i], vec[i + 1]; vec[i], vec[i + 1] = a * fr - b * fi, a * fi + b * fr
            kc[l].append(k); vc[l].append(v)
            xb = []
            for h in range(HEADS):
                g = (h // kv_mul) * hs
                att = [sum(q[h * hs + i] * kc[l][t][g + i] for i in range(hs)) / math.sqrt(hs) for t in range(pos + 1)]
                m = max(att); e = [math.exp(a - m) for a in att]; s = sum(e)
                xb += [sum(e[t] / s * vc[l][t][g + i] for t in range(pos + 1)) for i in range(hs)]
            x = [a + b for a, b in zip(x, mv(w["wo"], l * DIM * DIM, xb, DIM))]
            xb = rms(x, w["rms_ffn"], l * DIM)
            h1, h3 = mv(w["w1"], l * DIM * HID, xb, HID), mv(w["w3"], l * DIM * HID, xb, HID)
            x = [a + b for a, b in zip(x, mv(w["w2"], l * HID * DIM, [a / (1 + math.exp(-a)) * b for a, b in zip(h1, h3)], DIM))]
        x = rms(x, w["rms_final"], 0)
        if pos < len(toks) - 1: nxt = toks[pos + 1]
        else:
            lg = mv(w["wcls"], 0, x, V); order = sorted(range(V), key=lambda i: -lg[i])
            nxt = order[0]; margin = min(margin, lg[order[0]] - lg[order[1]])
        pos += 1
        if nxt == 1: break
        piece = vocab[nxt]
        if token == 1 and piece.startswith(" "): piece = piece[1:]
        out += chr(int(piece[3:5], 16)) if re.fullmatch(r"<0x[0-9A-F]{2}>", piece) else piece
        token = nxt
    return out, pos, margin

def squash(s):   # what is left to compare after the Console wraps lines: no whitespace, non-printables as '?'
    return "".join(ch if 32 < ord(ch) < 127 else ("?" if ch not in " \n" else "") for ch in s)

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-llm-")
seed = 820   # the first seed that qualifies, found by running this loop from 1
while True:   # a seed where no greedy pick is a near tie, so float32 rounding in the kernel cannot change a token
    model, tok, w, vocab, scores = make_model(seed)
    refs = {p: reference(w, vocab, scores, p) for p in (PROMPT, QUESTION)}
    varied = all(len(set(r[0][len(p):])) >= 8 for p, r in refs.items())   # not one token over and over
    if min(r[2] for r in refs.values()) > 0.05 and varied: break
    seed += 1
print("arm64 llm: tiny random checkpoint (seed %d, %d bytes), the reference text for %r: %r" % (seed, len(model), PROMPT, refs[PROMPT][0][:60]))
open(tmp + "/model.bin", "wb").write(model); open(tmp + "/tokenizer.bin", "wb").write(tok)
token_file = tmp + "/token"
open(token_file, "w").write("arm-llm-check-token-0123456789\n")
port = free_port()   # nothing listens here: the relay is unreachable

def build(extra="", pad=0):
    open(tmp + "/model.bin", "wb").write(model + bytes(pad))   # zeros after the weights: llm.c ignores them, the image grows
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
    env.update(CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf", "LLM_MODEL=" + tmp + "/model.bin", "LLM_TOK=" + tmp + "/tokenizer.bin",
                        "LLM_CFLAGS=" + extra], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)

class Boot:
    def __init__(self, name, net):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        nic = ["-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n"] if net else ["-nic", "none"]
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", *nic, "-device", "ramfb",
                                   "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.watchdog = threading.Timer(240, self.q.kill); self.watchdog.daemon = True; self.watchdog.start()   # a hard timeout on every run
        if not self.wait_for("M2 input ready", 30): self.close(); raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        if net: self.wait_for("net dhcp", 15)
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, pattern, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if len(re.findall(pattern, self.uart())) >= count: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def ask(self, text):
        for ch in text:
            self.cmd("send-key", keys=[{"type": "qcode", "data": "spc" if ch == " " else ch}]); time.sleep(0.12)
        self.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
    def close(self):
        self.watchdog.cancel(); self.q.kill(); self.q.wait()

DONE = r"llm: (\d+) tokens, (\d+)\.(\d) tok/s"
def answer(out, n):   # the n-th local answer: (its text, positions, tok/s)
    chunk = out.split("llm: thinking\n")[n]
    m = re.search(DONE, chunk)
    return (chunk[:m.start()], int(m.group(1)), m.group(2) + "." + m.group(3)) if m else (chunk, -1, "")

def same(got, ref): return squash(got[0]) == squash(ref[0]) and got[1] == ref[1]

speeds = []
try:
    build()
    b = Boot("net", True)
    try:
        b.ask("llm " + PROMPT)
        b.wait_for(DONE, 60)
        got = answer(b.uart(), 1)
        speeds.append(got[2])
        check("`llm %s` writes the reference model's text, token for token" % PROMPT, same(got, refs[PROMPT]),
              "kernel %r (%d) vs reference %r (%d)" % (got[0][:80], got[1], refs[PROMPT][0][:80], refs[PROMPT][1]))
        check("it prints tokens and tokens per second", got[1] > 0 and got[2] != "", got[2])
        b.ask(QUESTION)
        b.wait_for(DONE, 60, 2)
        out = b.uart()
        check("a question with the relay not listening: claude reports the error", re.search(r"claude: (error|timeout)", out) is not None, out[-300:])
        got = answer(out, 2)
        check("then the local model answers it", same(got, refs[QUESTION]), "%r vs %r" % (got[0][:80], refs[QUESTION][0][:80]))
    finally: b.close()

    build(pad=64 << 20)
    b = Boot("nonet", False)
    try:
        b.ask(QUESTION)
        b.wait_for(DONE, 60)
        out = b.uart()
        check("no network card: \"claude: no network\" first", "claude: no network\nllm: thinking" in out, out[-300:])
        got = answer(out, 1)
        speeds.append(got[2])
        check("then the local model answers, from an image padded past 64 MiB", same(got, refs[QUESTION]), "%r vs %r" % (got[0][:80], refs[QUESTION][0][:80]))
    finally: b.close()

    build("-DLLM_BREAK")
    b = Boot("broken", False)
    try:
        b.ask("llm " + PROMPT)
        b.wait_for(DONE, 60)
        got = answer(b.uart(), 1)
        check("the LLM_BREAK build (no rotary positions) does not match: the check discriminates",
              got[1] > 0 and not same(got, refs[PROMPT]), "%r" % got[0][:80])
    finally: b.close()
finally:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the kernel runs the llama2.c forward pass exactly (tiny random checkpoint, QEMU, %s tok/s), `llm` answers at the ask> row, "
      "and Samantha falls back to it when the relay is unreachable" % " and ".join(speeds))
