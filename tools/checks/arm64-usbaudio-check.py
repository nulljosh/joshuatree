#!/usr/bin/env python3
"""ARM64 USB audio: the aarch64 kernel finds a UAC1 speaker behind an xHCI controller, picks its 16-bit stereo alternate
setting, configures an isochronous OUT endpoint and streams 440 Hz to it, one transfer per millisecond.

QEMU's usb-audio is a speaker only, so this proves the half of the driver both directions share: the descriptor walk
(format type, rates, the data endpoint), Set Interface and the sampling-frequency request, the isochronous endpoint
context, isochronous TRBs with start-in-next-frame, and the event path that queues the next transfer. The microphone
(isochronous IN) changes only the endpoint type, the TRB flags and which way the bytes are copied; it needs the real Pi
and a real mic.

The usbaudiotest kernel plays 440 Hz for half a second at boot. QEMU writes what the device received to a WAV file; the
check wants a half second of stereo audio at 48 kHz (or 44.1) in which the zero crossings give 440 Hz, and the UART
saying "usb audio out: N frames played, 0 errors".
Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-usbaudio-check.py   (from the repo root)
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time, wave

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", arch, "usbaudiotest"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make usbaudiotest` does not build"); sys.exit(1)

tmp = tempfile.mkdtemp()
log, wav = tmp + "/uart", tmp + "/out.wav"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                      "-device", "pcie-root-port,id=rp,chassis=1",
                      "-device", "qemu-xhci,id=x,bus=rp",
                      "-audiodev", "wav,id=snd,path=" + wav,
                      "-device", "usb-audio,bus=x.0,audiodev=snd",
                      "-display", "none", "-serial", "file:" + log,
                      "-kernel", os.path.join(arch, "usbaudio-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(pat, tries=300):
    for _ in range(tries):
        if re.search(pat, uart()): return True
        time.sleep(0.1)
    return False
try:
    m = None
    if not wait_for(r"usb audio out addr \d+ .* Hz \d ch ep"):
        fails.append(f"no 'usb audio out' line: the speaker was not configured, got {uart()[-1200:]!r}")
    else:
        m = re.search(r"usb audio out addr \d+ port \S+ (\d+) Hz (\d) ch ep ([0-9a-f]+) mps (\d+)", uart())
        print(f"  ok: the speaker was found and configured: {m.group(0)}" if m else "  ok: speaker line")
        if wait_for(r"usb audio out: \d+ frames played, \d+ errors", 150):
            done = re.search(r"usb audio out: (\d+) frames played, (\d+) errors", uart())
            print(f"  ok: {done.group(0)}")
            if int(done.group(2)): fails.append(f"isochronous transfers reported errors: {done.group(0)}")
            rate = int(m.group(1)) if m else 48000
            if int(done.group(1)) != rate // 2: fails.append(f"expected {rate // 2} frames for half a second, got {done.group(1)}")
        else:
            fails.append(f"the stream never finished, got {uart()[-600:]!r}")
finally:
    q.terminate()
    try: q.wait(5)
    except Exception: q.kill()
if os.path.exists(wav) and os.path.getsize(wav) > 44:
    raw = open(wav, "rb").read()
    hdr = raw.find(b"data")
    pcm = raw[hdr + 8:]
    n = len(pcm) // 4
    s = struct.unpack("<%dh" % (n * 2), pcm[:n * 4])
    left = s[0::2]
    loud = [i for i, v in enumerate(left) if abs(v) > 2000]
    if len(loud) < 4000: fails.append(f"QEMU's WAV holds {len(loud)} loud samples, want a half second of tone")
    else:
        seg = left[loud[0]:loud[-1] + 1]
        zc = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
        rate = struct.unpack("<I", raw[24:28])[0]   # QEMU's WAV backend resamples to its own rate (44.1 kHz)
        hz = zc / 2 * rate / len(seg)
        print(f"  ok: QEMU's WAV has {len(seg)} frames of tone, about {hz:.0f} Hz")
        if not 400 <= hz <= 480: fails.append(f"the tone in the WAV is {hz:.0f} Hz, want about 440")
else:
    fails.append("QEMU wrote no audio: the device never received a transfer")
shutil.rmtree(tmp, ignore_errors=True)
if fails:
    for f in fails: print("FAIL:", f)
    sys.exit(1)
print("OK: USB audio streams over isochronous transfers")
