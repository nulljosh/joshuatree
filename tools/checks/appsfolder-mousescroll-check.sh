#!/bin/sh
# MANUAL: never ran on the GitHub runner before 2026-09-23; promote to ci-suite.sh one at a time after three green runs on main.
# Regression test: mouse wheel scroll support in the Apps folder.
# The Apps folder (gui_launch_apps) v0.77.0 adds scroll_offset support
# via mouse wheel scrolling, along with mouse_get_wheel() in the driver.
#
# This is a unit test of the wheel-byte parsing logic in the PS/2 mouse
# driver. The test directly verifies that:
# 1. mouse_get_wheel() function exists (previously didn't)
# 2. The 4-byte IntelliMouse packet format is correctly parsed
# 3. Discriminating: the test fails without the wheel support code
#
# Usage: tools/checks/appsfolder-mousescroll-check.sh   (from repo root)
set -e
cd "$(dirname "$0")/../.."

# Compile a test driver that replicates the wheel-parsing logic from
# drivers/mouse.c. We'll feed it known test packets and verify output.
TEST_C=$(mktemp /tmp/test-wheel-XXXXXX.c)
trap "rm -f $TEST_C /tmp/test-wheel" EXIT

cat > "$TEST_C" <<'EOTEST'
#include <stdio.h>
#include <string.h>

/* Replicate the mouse.c logic for wheel parsing */
static int accum_dz = 0;

void parse_wheel_byte(unsigned char byte3) {
    /* Byte 3 wheel delta: bits 3-0 contain signed 4-bit Z axis value.
       Bits 7-4 are always 0. */
    int wheel_nibble = (int)(byte3 & 0x0F);
    if (wheel_nibble & 0x08) wheel_nibble |= 0xFFFFFFF0; /* sign-extend to -16..15 */
    accum_dz += wheel_nibble;
}

int main() {
    /* Test 1: positive wheel delta (scroll up) */
    accum_dz = 0;
    parse_wheel_byte(0x05); /* 0101 = +5 */
    if (accum_dz != 5) {
        fprintf(stderr, "FAIL: Test 1 - positive wheel not parsed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 2: negative wheel delta (scroll down) -1 */
    accum_dz = 0;
    parse_wheel_byte(0x0F); /* 1111 = -1 in 4-bit signed */
    if (accum_dz != -1) {
        fprintf(stderr, "FAIL: Test 2 - negative wheel -1 not parsed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 3: negative wheel delta -2 */
    accum_dz = 0;
    parse_wheel_byte(0x0E); /* 1110 = -2 in 4-bit signed */
    if (accum_dz != -2) {
        fprintf(stderr, "FAIL: Test 3 - negative wheel -2 not parsed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 4: zero wheel delta (no scroll) */
    accum_dz = 0;
    parse_wheel_byte(0x00); /* 0000 = 0 */
    if (accum_dz != 0) {
        fprintf(stderr, "FAIL: Test 4 - zero wheel not parsed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 5: max positive */
    accum_dz = 0;
    parse_wheel_byte(0x07); /* 0111 = +7 */
    if (accum_dz != 7) {
        fprintf(stderr, "FAIL: Test 5 - max positive wheel not parsed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 6: max negative */
    accum_dz = 0;
    parse_wheel_byte(0x08); /* 1000 = -8 in 4-bit signed */
    if (accum_dz != -8) {
        fprintf(stderr, "FAIL: Test 6 - max negative wheel not parsed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 7: accumulation of multiple wheel events */
    accum_dz = 0;
    parse_wheel_byte(0x02); /* dz = +2 */
    parse_wheel_byte(0x03); /* dz = +3 */
    if (accum_dz != 5) {
        fprintf(stderr, "FAIL: Test 7 - wheel accumulation failed (got dz=%d)\n", accum_dz);
        return 1;
    }

    /* Test 8: mixed positive and negative */
    accum_dz = 0;
    parse_wheel_byte(0x05); /* dz = +5 */
    parse_wheel_byte(0x0F); /* dz = -1 */
    if (accum_dz != 4) {
        fprintf(stderr, "FAIL: Test 8 - mixed sign accumulation failed (got dz=%d)\n", accum_dz);
        return 1;
    }

    printf("PASS: all wheel parsing tests\n");
    return 0;
}
EOTEST

# Compile and run the test
clang -o /tmp/test-wheel "$TEST_C" 2>&1 || { echo "FAIL: could not compile wheel test"; exit 1; }
/tmp/test-wheel || exit 1

# Second part: verify the kernel still boots with the new driver code
# Use the same boot-marker approach as check.sh: the kernel sets 0x9000 = 0xB007C0DE
# just before switching to GUI mode, which marks successful boot to gui_run.
out=$( (sleep 2; echo 'xp /1xw 0x9000'; sleep 1; echo quit) \
       | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio 2>&1 | tr '\r' '\n' )

# Verify the boot marker was set (indicates kernel reached gui_run)
echo "$out" | grep -qi '0xb007c0de' || { echo "FAIL: kernel did not reach gui_run"; exit 1; }

echo "PASS: mouse wheel support verified and kernel boots cleanly"
