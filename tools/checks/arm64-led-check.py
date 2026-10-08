#!/usr/bin/env python3
"""The Pi's green activity light is blinked through the VideoCore mailbox, not the BCM2711 GPIO registers.

Static check, no hardware: arch/arm64/main.c must ask the firmware for "set GPIO state" (tag 0x00038041) on pin 42.
led_blink must never run at boot (a boot-time blink left the USB keyboard dead on the real Pi), and it must not wait
on `ticks`, which stops at 3, so a wait on it never ends. Samantha's [[led blink]] (ask.c) is its only caller.
"""
import re, sys
src = open("arch/arm64/main.c").read()
ok = True
if not re.search(r"unsigned m\[\] = \{ 8 \* 4, 0, 0x00038041, 8, 0, 42, state, 0 \}", src):
    print("FAIL: led_set does not send tag 0x00038041 for pin 42"); ok = False
if re.search(r"^\s+led_blink\(", src, re.M):
    print("FAIL: main.c calls led_blink (the boot path must not blink)"); ok = False
body = re.search(r"static void led_wait\(void\) \{.*?\n\}", src, re.S)
code = re.sub(r"/\*.*?\*/", "", body.group(0), flags=re.S) if body else ""
if "cntpct_el0" not in code or re.search(r"\bticks\b", code):
    print("FAIL: led_blink's wait is not on the generic counter"); ok = False
if "led_blink(1)" not in open("arch/arm64/ask.c").read():
    print("FAIL: ask.c does not run [[led blink]]"); ok = False
if ok:
    print("PASS: the green light goes through the firmware mailbox (pin 42), never at boot, on a wait that ends")
sys.exit(0 if ok else 1)
