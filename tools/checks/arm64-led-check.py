#!/usr/bin/env python3
"""The Pi's green activity light is driven through the BCM2711 GPIO registers (pin 42 is on the chip on a Pi 4, not on the firmware expander).

Static check, no hardware: arch/arm64/main.c must set GPIO 42 to output (GPFSEL4) and write GPSET1 / GPCLR1 bit 10.
led_blink must never run at boot (a boot-time blink left the USB keyboard dead on the real Pi), and it must not wait
on `ticks`, which stops at 3, so a wait on it never ends. Samantha's [[led blink]] (ask.c) is its only caller.
"""
import re, sys
src = open("arch/arm64/main.c").read()
ok = True
if not (re.search(r"GPIO_BASE \+ 0x10", src) and re.search(r"GPIO_BASE \+ \(state \? 0x20 : 0x2C\)\) = 1u << 10", src)):
    print("FAIL: led_set does not drive GPIO 42 through GPFSEL4 and GPSET1/GPCLR1 bit 10"); ok = False
if re.search(r"^\s+led_blink\(", src, re.M):
    print("FAIL: main.c calls led_blink (the boot path must not blink)"); ok = False
body = re.search(r"static void led_wait\(void\) \{.*?\n\}", src, re.S)
code = re.sub(r"/\*.*?\*/", "", body.group(0), flags=re.S) if body else ""
if "cntpct_el0" not in code or re.search(r"\bticks\b", code):
    print("FAIL: led_blink's wait is not on the generic counter"); ok = False
if "led_blink(1)" not in open("arch/arm64/ask.c").read():
    print("FAIL: ask.c does not run [[led blink]]"); ok = False
if ok:
    print("PASS: the green light is driven through GPIO registers (pin 42), never at boot, on a wait that ends")
sys.exit(0 if ok else 1)
