#!/usr/bin/env python3
"""Sound Blaster 16: the kernel finds the card and `beep` really plays.

Boots QEMU headless (-display none, never a window) with an SB16 wired to
QEMU's wav audio backend, types `beep` into the text shell over the monitor
(the same sendkey path tools/checks/shellregress-check.sh uses), then reads
the wav QEMU wrote. Asserts three things:

1. serial says the DSP answered its reset ("sb16: found").
2. the wav holds real, non-silent audio (RMS well above the noise floor).
3. that audio is a 440Hz tone: counted zero crossings put the pitch within
   a few percent of 440Hz, and a Goertzel probe finds far more energy at
   440Hz than at 300Hz or 600Hz.

A second, card-less boot asserts the driver is a silent no-op when the
card is absent: serial says "sb16: not found" and the boot still reaches
the shell (no hang in the detect path).
"""
import math, os, struct, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL)


def keys(text):
    out = []
    for c in text:
        out.append("sendkey spc" if c == " " else f"sendkey {c}")
    out.append("sendkey ret")
    return out


def boot(extra, work, commands):
    serial = os.path.join(work, "serial.txt")
    q = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none",
         "-monitor", "stdio", "-serial", f"file:{serial}", *extra],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    try:
        time.sleep(4)
        q.stdin.write("sendkey ctrl-alt-backspace\n"); q.stdin.flush()   # leave the GUI for the text shell (plain esc on a bare desktop is now a no-op, kernel.c gui_run)
        time.sleep(1)
        for line in commands:
            q.stdin.write(line + "\n"); q.stdin.flush()
            time.sleep(0.05)
        time.sleep(3)                                      # half a second of tone, plus slack
        q.stdin.write("quit\n"); q.stdin.flush()
        q.wait(timeout=15)
    finally:
        if q.poll() is None:
            q.kill()
    return open(serial, errors="replace").read() if os.path.exists(serial) else ""


def goertzel(samples, rate, freq):
    w = 2 * math.pi * freq / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2


def zero_crossing_pitch(tone, secs):
    """Schmitt-trigger crossing count: a bare mean-crossing count double-fires
    on a stray ripple near zero (seen under some CI QEMU builds' audio
    resampling), so require a real swing through a +-30% amplitude band
    before it arms the next crossing."""
    mean = sum(tone) / len(tone)
    amp = (max(tone) - min(tone)) / 2
    lo, hi = mean - 0.3 * amp, mean + 0.3 * amp
    armed = tone[0] < lo
    crossings = 0
    for v in tone:
        if armed and v > hi:
            crossings += 1
            armed = False
        elif not armed and v < lo:
            armed = True
    return crossings / secs


fails = []
with tempfile.TemporaryDirectory(prefix="jt-sb16-") as work:
    work = os.environ.get("SB16_KEEP", work)
    wav_path = os.path.join(work, "out.wav")
    log = boot(["-audiodev", f"wav,id=snd,path={wav_path}", "-device", "sb16,audiodev=snd"],
               work, keys("beep"))
    if "sb16: found" not in log:
        fails.append("serial never reported the card (no 'sb16: found')")
    if "sb16: beep" not in log:
        fails.append("the beep command never reached the driver (no 'sb16: beep' on serial)")
    if "timed out" in log:
        fails.append("the DMA transfer's IRQ 5 never arrived (serial: transfer timed out)")

    samples, rate = [], 0
    if os.path.exists(wav_path) and os.path.getsize(wav_path) > 44:
        # Parse the header by hand: QEMU's wav backend only patches the RIFF
        # and data sizes on a clean audio shutdown, which a monitor `quit`
        # skips, so both read 0 and the wave module refuses the file.
        raw = open(wav_path, "rb").read()
        ch, rate = struct.unpack_from("<HI", raw, 22)
        width = struct.unpack_from("<H", raw, 34)[0] // 8
        raw = raw[44:]
        raw = raw[:len(raw) - len(raw) % (width * ch)]
        if width == 2:
            vals = struct.unpack(f"<{len(raw)//2}h", raw)
        else:
            vals = [b - 128 for b in raw]
        samples = [float(v) for v in vals[::ch]]              # left channel
    if not samples:
        fails.append("QEMU wrote no audio at all")
    else:
        # Keep only the loud stretch, so leading/trailing silence doesn't
        # dilute the measurement.
        loud = [i for i, v in enumerate(samples) if abs(v) > 500]
        tone = samples[loud[0]:loud[-1] + 1] if loud else []
        rms = math.sqrt(sum(v * v for v in tone) / len(tone)) if tone else 0.0
        secs = len(tone) / rate if rate else 0
        print(f"sb16-check: wav {rate}Hz, loud stretch {secs:.2f}s, rms {rms:.0f}")
        if rms < 1000 or secs < 0.3:
            fails.append(f"audio is silent or too short (rms {rms:.0f}, {secs:.2f}s)")
        else:
            pitch = zero_crossing_pitch(tone, secs)
            seg = tone[:rate // 4]
            e440, e300, e600 = (goertzel(seg, rate, f) for f in (440, 300, 600))
            print(f"sb16-check: pitch by zero crossings {pitch:.1f}Hz, "
                  f"energy 440/300 = {e440 / max(e300, 1):.0f}x, 440/600 = {e440 / max(e600, 1):.0f}x")
            if abs(pitch - 440) > 20:
                fails.append(f"tone is {pitch:.1f}Hz, not 440Hz")
            if e440 < 20 * e300 or e440 < 20 * e600:
                fails.append("energy is not concentrated at 440Hz")

    # Card absent: detect must fail fast and quietly, and boot must go on.
    log2 = boot([], work + "/", keys("beep"))
    if "sb16: not found" not in log2:
        fails.append("card-less boot did not report 'sb16: not found'")
    if "sb16: beep" in log2:
        fails.append("card-less beep still tried to play")

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)
print("sb16-check: OK, card detected, beep played a real 440Hz tone, card-less boot is a no-op")
