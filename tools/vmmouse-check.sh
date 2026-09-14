#!/bin/sh
# Headless proof of the VMware absolute-pointer path against QEMU's OWN
# vmmouse device (hw/input/vmmouse.c, created by default alongside the pc
# machine's vmport), no display window needed. Boots kernel.elf with
# -display none and drives QEMU over QMP on stdio:
#   query-mice        -> once the guest has requested absolute mode, QEMU
#                        itself lists "vmmouse" as current and absolute
#   input-send-event  -> a real ABSOLUTE pointer event (axis 0..32767),
#                        the same event a display backend generates; the
#                        kernel's serial log then shows the exact packet
#                        (QEMU shifts <<1, so 16384/24576 arrive as
#                        32768/49152), plus a left press and release.
# HMP's `mouse_move` is deliberately NOT used: it only emits RELATIVE
# events, which QEMU routes to the PS/2 handler, not the vmmouse, found
# the hard way while writing this (the kernel then correctly ignored
# them, which is its own small proof of the PS/2-ignore guard).
#
# Why this exists: roadmap.md's honest note for the PS/2 path is that the
# monitor's mouse commands never reached the kernel's PS/2 driver
# headlessly. The backdoor changes that, so the native QEMU side can be
# verified from a script for the first time.
#
# Usage: tools/vmmouse-check.sh   (from the repo root, after make kernel.elf)
set -e
cd "$(dirname "$0")/.."
LOG=/tmp/jt-vmmouse-serial.log
rm -f "$LOG"
out=$( ( echo '{"execute":"qmp_capabilities"}'; sleep 5;
         echo '{"execute":"query-mice"}'; sleep 1;
         echo '{"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":16384}},{"type":"abs","data":{"axis":"y","value":24576}}]}}'; sleep 1;
         echo '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}'; sleep 1;
         echo '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}'; sleep 1;
         echo '{"execute":"quit"}' ) \
       | qemu-system-i386 -kernel kernel.elf -display none -qmp stdio -serial "file:$LOG" 2>&1 )
echo "--- QMP query-mice"
echo "$out" | grep -o '"name": *"[^"]*"[^}]*' | head -4
echo "--- serial (vmmouse lines)"
grep -i "vmmouse" "$LOG" || true
fail=0
grep -q "vmmouse_init: VMware backdoor answered" "$LOG" || { echo "FAIL: kernel did not detect the backdoor"; fail=1; }
echo "$out" | grep -q '"name": *"vmmouse".*"absolute": *true\|"absolute": *true.*"name": *"vmmouse"' || { echo "FAIL: QEMU does not list the vmmouse as the absolute pointer"; fail=1; }
grep -q "vmmouse: first absolute packet x=32768 y=49152" "$LOG" || { echo "FAIL: kernel did not receive the injected absolute position"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: QEMU vmmouse absolute pointer round trip" || exit 1

# Phase 2, the fallback: the same kernel on a machine with NO backdoor
# (-machine vmport=off removes both vmport and vmmouse, the closest a
# QEMU run gets to bare hardware) must say so and still reach gui_run,
# PS/2 relative pointer only. This is the guard for the native app's
# real -display cocoa window and for actual hardware, never to be broken
# by the absolute path.
rm -f "$LOG"
out=$( (sleep 3; echo 'xp /1xw 0x9000'; sleep 1; echo quit) \
       | qemu-system-i386 -kernel kernel.elf -machine vmport=off -display none -monitor stdio -serial "file:$LOG" 2>&1 | tr '\r' '\n' )
echo "--- serial (vmport=off)"
grep -i "vmmouse" "$LOG" || true
grep -q "vmmouse_init: no backdoor, PS/2 relative pointer only" "$LOG" || { echo "FAIL: fallback kernel did not report the missing backdoor"; exit 1; }
echo "$out" | grep -qi '0xb007c0de' || { echo "FAIL: fallback kernel did not reach gui_run"; exit 1; }
echo "PASS: no backdoor -> PS/2 relative fallback, boot marker reached"
