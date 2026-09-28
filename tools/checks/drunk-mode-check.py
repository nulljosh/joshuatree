#!/usr/bin/env python3
"""Easter egg test: drunk mode applies horizontal sway to framebuffer rows.

Boot with cmdline `drunk` flag, then take two screenshots and assert that
the row offsets differ between frames due to the sway animation.

Usage: tools/checks/drunk-mode-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time

LOG = "/tmp/jt-drunk-serial.log"
PORT = 4711
FB = 0xfd000000; W, H = 1920, 1080

def qmp_command(sock, cmd):
    sock.sendall(json.dumps({"execute": cmd}).encode())
    sock.recv(4096)

def screenshot(sock, dump_path):
    qmp_command(sock, "stop")
    time.sleep(0.1)
    qmp_command(sock, f"pmemsave {FB} {W*H*4} {dump_path}")
    time.sleep(0.2)
    qmp_command(sock, "cont")
    time.sleep(0.1)

def check_horizontal_variance(img_bytes):
    """Returns True if row offsets vary, indicating sway is working."""
    pixels = [int.from_bytes(img_bytes[i:i+4], 'little') for i in range(0, len(img_bytes), 4)]
    rows = [pixels[y*W:(y+1)*W] for y in range(H)]

    variances = []
    for y in range(1, min(100, H)):
        prev_row = rows[y-1]
        curr_row = rows[y]
        if prev_row and curr_row:
            variance = sum(1 for i in range(W-10) if prev_row[i] != curr_row[i])
            variances.append(variance)

    return any(v > W*0.1 for v in variances)

try:
    if os.path.exists(LOG): os.remove(LOG)

    kernel = "kernel/kernel.elf"
    if not os.path.exists(kernel):
        print(f"FAIL: {kernel} not found (run 'make kernel.elf' first)")
        sys.exit(1)

    with open("/tmp/jt-drunk-qmp", "w") as f:
        qemu = subprocess.Popen([
            "qemu-system-i386",
            "-machine", "pc", "-m", "128",
            "-kernel", kernel,
            "-append", "drunk",
            "-qmp", f"unix:/tmp/jt-drunk-qmp,server,nowait",
            "-serial", f"file:{LOG}",
            "-nographic", "-monitor", "none",
            "-device", "sb16", "-audiodev", "none,id=a0"
        ])

    time.sleep(2)
    sock = socket.socket(socket.AF_UNIX)
    sock.connect("/tmp/jt-drunk-qmp")
    sock.recv(4096)

    qmp_command(sock, "cont")
    time.sleep(3)

    dump1 = "/tmp/jt-drunk-frame1.raw"
    dump2 = "/tmp/jt-drunk-frame2.raw"

    screenshot(sock, dump1)
    time.sleep(0.2)
    screenshot(sock, dump2)

    with open(dump1, 'rb') as f:
        img1 = f.read()
    with open(dump2, 'rb') as f:
        img2 = f.read()

    qmp_command(sock, "quit")
    qemu.wait(timeout=5)

    if len(img1) != W*H*4 or len(img2) != W*H*4:
        print(f"FAIL: dump size mismatch (got {len(img1)}, {len(img2)}, expected {W*H*4})")
        sys.exit(1)

    if img1 == img2:
        print("FAIL: frames identical, sway not applied")
        sys.exit(1)

    diff_pixels = sum(1 for i in range(len(img1)) if img1[i] != img2[i])
    if diff_pixels < W*H*0.01:
        print(f"FAIL: only {diff_pixels} pixels differ, expected more variance from sway")
        sys.exit(1)

    print(f"PASS: drunk mode sway detected ({diff_pixels} differing pixels)")

except Exception as e:
    print(f"FAIL: {e}")
    sys.exit(1)
