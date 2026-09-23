#!/usr/bin/env python3
"""Test Homeqi app: boot kernel, open Homeqi from Apps, test interaction."""
import subprocess, sys, time, socket, os
from PIL import Image

FB = 0xfd000000
W, H = 1920, 1080
PORT = 4462
LOG = "/tmp/jt-homeqi-test.log"
DUMP = "/tmp/jt-homeqi-test.raw"

def qemu_cmd(cmd):
    """Send command to QEMU monitor."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', PORT))
        s.send((cmd + "\n").encode())
        s.close()
        time.sleep(0.1)
    except:
        pass

def dump_fb(path):
    """Dump framebuffer to PNG."""
    qemu_cmd(f"pmemsave {FB} {W * H * 4} {DUMP}")
    time.sleep(0.2)
    try:
        with open(DUMP, 'rb') as f:
            data = f.read()
        img = Image.frombytes('RGBA', (W, H), data)
        img.save(path)
        print(f"Saved {path}")
    except:
        pass

def nav_and_test():
    """Boot kernel, navigate to Homeqi, test it."""
    # Boot
    print("Starting kernel...")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf",
         "-serial", f"file:{LOG}", "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
         "-display", "none", "-m", "256"],
        cwd="/tmp/jt-loop/homeqi"
    )
    time.sleep(3)

    # Wait for desktop
    for _ in range(60):
        time.sleep(0.5)
        try:
            with open(LOG, 'r') as f:
                if "desktop ready" in f.read().lower():
                    break
        except:
            pass

    time.sleep(2)

    # Open Apps folder (dock slot 25 = Apps icon)
    print("Opening Apps folder...")
    qemu_cmd("sendkey tab")  # Focus dock
    for _ in range(25):
        qemu_cmd("sendkey right")  # Navigate to Apps
        time.sleep(0.05)
    qemu_cmd("sendkey ret")  # Open Apps
    time.sleep(1)

    # Navigate to Homeqi (icon 16, in grid: row 3, col 1)
    # Grid is 5 wide, so icon 16 = row 3, col 1
    # Navigation from top-left: right 1, down 3
    print("Navigating to Homeqi (icon 16)...")
    qemu_cmd("sendkey right")  # col 1
    time.sleep(0.1)
    for _ in range(3):
        qemu_cmd("sendkey down")  # row 3
        time.sleep(0.1)

    time.sleep(0.5)
    dump_fb("/tmp/jt-homeqi-gallery/16-Homeqi-question.png")

    # Press 1 (yes)
    print("Answering question 1 with yes...")
    qemu_cmd("sendkey 1")
    time.sleep(0.5)
    dump_fb("/tmp/jt-homeqi-gallery/16-Homeqi-reasoning.png")

    # Press space to continue
    print("Continuing to next question...")
    qemu_cmd("sendkey space")
    time.sleep(0.5)

    # Keep pressing space/1 to advance through questions quickly
    for i in range(7):
        qemu_cmd("sendkey 1")
        time.sleep(0.3)
        qemu_cmd("sendkey space")
        time.sleep(0.3)

    time.sleep(0.5)
    dump_fb("/tmp/jt-homeqi-gallery/16-Homeqi-result.png")

    # Close
    print("Closing...")
    qemu_cmd("sendkey esc")
    time.sleep(0.5)

    proc.terminate()
    proc.wait(timeout=5)
    print("Done!")

if __name__ == "__main__":
    os.makedirs("/tmp/jt-homeqi-gallery", exist_ok=True)
    nav_and_test()
