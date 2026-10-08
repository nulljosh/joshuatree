#!/usr/bin/env python3
"""The Pi's green activity light is blinked through the VideoCore mailbox, not the BCM2711 GPIO registers.

Static check, no hardware: arch/arm64/main.c must ask the firmware for "set GPIO state" (tag 0x00038041) on pin 42,
and led_blink must run after the Wi-Fi bring-up at boot. Reverting the mailbox call or the boot call fails this check.
"""
import re, sys
src = open("arch/arm64/main.c").read()
ok = True
if not re.search(r"0x00038041", src) or not re.search(r"unsigned m\[\] = \{ 8 \* 4, 0, 0x00038041, 8, 0, 42, state, 0 \}", src):
    print("FAIL: led_set does not send tag 0x00038041 for pin 42"); ok = False
if not re.search(r"wifi_nic_up\(\)\) \{[^}]*\}\s*\n\s*led_blink\(3\);", src):
    print("FAIL: led_blink(3) is not called after the Wi-Fi bring-up at boot"); ok = False
if ok:
    print("PASS: the green light is driven through the firmware mailbox (pin 42) and blinks at boot")
sys.exit(0 if ok else 1)
