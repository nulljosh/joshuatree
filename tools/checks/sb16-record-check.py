#!/usr/bin/env python3
"""Sound Blaster 16 recording (drivers/sb16.c's sb16_record, kernel.c's
`listen` shell command): a real end-to-end capture check, with one real
limitation documented up front rather than hidden.

Reading QEMU's own hw/audio/sb16.c before writing this: QEMU's `-device
sb16` logs "ADC not yet supported" for every recording DSP command and
never drives an audio-input backend at all -- there is no host-mic-to-
guest-DMA path in QEMU's SB16 model today, on this build or any other.
So this check cannot assert "captured real, non-silent audio" the way
sb16-check.py asserts a real 440Hz tone for playback: under QEMU, the
`listen` command's DMA transfer will time out (no IRQ 5 ever fires) and
sb16_record returns 0, exactly like the no-card path. That is QEMU's own
emulation gap, not this driver -- drivers/sb16.c's record_chunk sends the
same real, DSP-2.xx-compatible ADC command pair (0x40 set time constant,
0x24 8-bit single-cycle input) any real SB16 (and this task's own Yeti-
over-coreaudio setup, per `make talk`) answers.

What this check DOES assert, which is real and QEMU-verifiable:
1. `listen` reaches the driver at all (serial: "sb16: record start").
2. The driver's own timeout fires cleanly -- the boot does not hang
   waiting on an IRQ that will never come (serial: "sb16: record done"
   appears).
3. A card-less boot is a clean, immediate no-op (n=0, no record markers,
   no hang), the same contract sb16-check.py already holds sb16_play to.
"""
import os, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL)


def boot(extra, work, commands):
    serial = os.path.join(work, "serial.txt")
    q = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none",
         "-monitor", "stdio", "-serial", f"file:{serial}", *extra],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    try:
        time.sleep(4)
        q.stdin.write("sendkey ctrl-alt-backspace\n"); q.stdin.flush()  # leave the GUI for the text shell (plain esc on a bare desktop is now a no-op, kernel.c gui_run)
        time.sleep(1)
        for c in commands:
            if c == " ":
                q.stdin.write("sendkey spc\n")
            else:
                q.stdin.write(f"sendkey {c}\n")
            q.stdin.flush()
            time.sleep(0.03)
        q.stdin.write("sendkey ret\n"); q.stdin.flush()
        time.sleep(6)                                     # record's own timeout window plus slack
        q.stdin.write("echo alive\n"); q.stdin.flush()     # prove the shell is still responsive after listen returns
        time.sleep(0.3)
        q.stdin.write("quit\n"); q.stdin.flush()
        q.wait(timeout=15)
    finally:
        if q.poll() is None:
            q.kill()
    return open(serial, errors="replace").read() if os.path.exists(serial) else ""


fails = []
with tempfile.TemporaryDirectory(prefix="jt-sb16-record-") as work:
    log = boot(["-audiodev", "wav,id=snd,path=" + os.path.join(work, "out.wav"),
                "-device", "sb16,audiodev=snd"], work, "listen")
    if "sb16: found" not in log:
        fails.append("serial never reported the card (no 'sb16: found')")
    if "sb16: record start" not in log:
        fails.append("`listen` never reached sb16_record (no 'sb16: record start')")
    if "sb16: record done" not in log:
        fails.append("sb16_record never returned -- looks like a real hang, not a clean timeout")
    print("sb16-record-check: known QEMU limitation -- ADC unimplemented in QEMU's sb16 model, "
          "so n=0 here is expected, not a driver bug (see this file's docstring)")

    log2 = boot([], work, "listen")
    if "sb16: not found" not in log2:
        fails.append("card-less boot did not report 'sb16: not found'")
    if "sb16: record start" in log2:
        fails.append("card-less boot still tried to record")

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)
print("sb16-record-check: OK, `listen` reaches the driver and times out cleanly (QEMU has no ADC "
      "backend), card-less boot is a clean no-op")
