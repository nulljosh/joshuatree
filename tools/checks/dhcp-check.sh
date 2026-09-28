#!/bin/sh
# MANUAL: never ran on the GitHub runner before 2026-09-27; promote to ci-suite.sh one at a time after three green runs on main.
# Real regression guard for the DHCP client (drivers/net.c's dhcp_negotiate,
# wired into net_init). QEMU's own user-mode network (-net nic,model=rtl8139
# -net user, aka SLIRP) runs a real DHCP server at 10.0.2.2 that hands out
# 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3 -- the exact fixed values
# this kernel used to hardcode everywhere, which is what makes this testable
# headless without a fake server: a real DHCP round trip against a real
# (virtual) router.
#
# Two boots, both driven the same "boot the real kernel headlessly, read its
# own serial log" way tools/checks/wallboot-check.sh already uses:
#
#   dhcp    plain boot, no cmdline flags. net_init runs the DHCP
#           DISCOVER/OFFER/REQUEST/ACK exchange before the desktop's own
#           startup weather fetch calls net_init: must print
#           "dhcp: lease 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3" on serial, and
#           the weather fetch that follows (a real DNS lookup for
#           ip-api.com plus a real HTTP GET, see kernel.c's weather_fetch)
#           must still succeed (`geo=` line), proving the leased IP/gateway
#           didn't break the rest of the stack.
#   nodhcp  `-append nodhcp`. Must print "dhcp: skipped (nodhcp)" instead
#           of ever attempting a lease, and net_init must fall straight
#           through to the old hardcoded 10.0.2.15 path -- proven the same
#           way, by the weather fetch's `geo=` line still succeeding.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

TMPROOT=$(mktemp -d /tmp/jt-dhcp-check-XXXX)
trap 'rm -rf "$TMPROOT"' EXIT

run_boot() {
    NAME="$1"; APPEND="$2"
    LOG="$TMPROOT/$NAME.log"
    ARGS="-name jt-dhcp-check-$NAME -kernel kernel.elf -display none -vga std -rtc base=localtime -net nic,model=rtl8139 -net user -serial file:$LOG -monitor none"
    if [ -n "$APPEND" ]; then ARGS="$ARGS -append $APPEND"; fi
    # shellcheck disable=SC2086
    qemu-system-i386 $ARGS &
    QPID=$!
    i=0
    while [ $i -lt 20 ]; do
        sleep 1
        if [ -f "$LOG" ] && grep -q '^geo=' "$LOG" 2>/dev/null; then break; fi
        i=$((i + 1))
    done
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    LC_ALL=C tr -d '\r' < "$LOG" > "$TMPROOT/$NAME.clean.log" 2>/dev/null || true
}

FAIL=0

run_boot "lease" ""
DHCP_LOG="$TMPROOT/lease.clean.log"
DHCP_LEASE=$(grep -m1 '^dhcp: lease ' "$DHCP_LOG" 2>/dev/null || true)
DHCP_GEO=$(grep -m1 '^geo=' "$DHCP_LOG" 2>/dev/null || true)
if [ "$DHCP_LEASE" != "dhcp: lease 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3" ]; then
    echo "FAIL: expected 'dhcp: lease 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3' on serial, got: '$DHCP_LEASE'"
    FAIL=1
else
    echo "PASS: DHCP lease line correct ($DHCP_LEASE)"
fi
if [ -z "$DHCP_GEO" ]; then
    echo "FAIL: no geo= line after DHCP -- DNS lookup + HTTP fetch broke after taking the lease"
    FAIL=1
else
    echo "PASS: DNS lookup + HTTP fetch still work after DHCP ($DHCP_GEO)"
fi

run_boot "nodhcp" "nodhcp"
NODHCP_LOG="$TMPROOT/nodhcp.clean.log"
NODHCP_SKIP=$(grep -m1 '^dhcp: skipped' "$NODHCP_LOG" 2>/dev/null || true)
NODHCP_LEASE=$(grep -m1 '^dhcp: lease' "$NODHCP_LOG" 2>/dev/null || true)
NODHCP_GEO=$(grep -m1 '^geo=' "$NODHCP_LOG" 2>/dev/null || true)
if [ -z "$NODHCP_SKIP" ]; then
    echo "FAIL: nodhcp never printed 'dhcp: skipped', got: '$(head -c200 "$NODHCP_LOG")'"
    FAIL=1
else
    echo "PASS: nodhcp skipped the DHCP exchange ($NODHCP_SKIP)"
fi
if [ -n "$NODHCP_LEASE" ]; then
    echo "FAIL: nodhcp still attempted/printed a lease: '$NODHCP_LEASE'"
    FAIL=1
fi
if [ -z "$NODHCP_GEO" ]; then
    echo "FAIL: no geo= line under nodhcp -- the old fixed-config path broke"
    FAIL=1
else
    echo "PASS: old fixed-config path still works under nodhcp ($NODHCP_GEO)"
fi

exit $FAIL
