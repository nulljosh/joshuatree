#!/bin/sh
# Headless, real-network proof that drivers/e1000.c (Intel 82540EM) carries
# the exact same stack rtl8139-check/geo-check.sh already proves over
# RTL8139: PCI probe, MMIO BAR, descriptor rings, MAC-from-EEPROM, then a
# real ARP resolution of the QEMU SLIRP gateway (10.0.2.2) followed by a
# real DNS+TCP+HTTP round trip (ip-api.com, then Open-Meteo), same as
# geo-check.sh does for RTL8139. net.c's own ARP/DNS/TCP code has no idea
# which NIC driver is underneath it (drivers/net.c's active_send/
# active_receive indirection); this boots with ONLY `-device e1000` on the
# QEMU command line (no rtl8139, no ne2k), so net_init() can only reach the
# weather fetch by successfully driving the e1000 hardware path all the way
# through. A kernel whose e1000 driver never gets past PCI probe, never
# resets the chip, or never gets a real frame onto the wire fails here
# exactly the way a broken RTL8139 driver fails geo-check.sh.
#
# QEMU's software (TCG) e1000 model costs noticeably more host CPU per
# guest instruction than its RTL8139 model (found by direct comparison: an
# otherwise-identical boot reaches the RTL8139 geo-check.sh's first wx= line
# in well under a minute, the same kernel under `-device e1000` can take
# several times that on a loaded host), so this check's wait budget is much
# larger than geo-check.sh's -- not a sign the driver is stuck, just a
# slower emulator underneath it. Needs internet access on the host and
# python3 (json parse of the curl body), same as geo-check.sh.
set -e
export LC_ALL=C # the kernel's weather text carries a CP437 degree byte (0xF8); BSD grep/cut/tr under a UTF-8 locale truncate or choke on it
cd "$(dirname "$0")/../.."
make -s kernel.elf
LOG=/tmp/jt-e1000-serial.log
rm -f "$LOG"
host=$(curl -s --max-time 10 http://ip-api.com/json/ | python3 -c 'import json,sys; d=json.load(sys.stdin); print(str(d["lat"])+","+str(d["lon"]))')
[ -n "$host" ] || { echo "FAIL: host could not reach ip-api.com"; exit 1; }
qemu-system-i386 -kernel kernel.elf -display none -vga std \
  -device e1000,netdev=n0 -netdev user,id=n0 -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 240); do
  sleep 1
  grep -q '^wx=' "$LOG" 2>/dev/null && break
done
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
geo=$(grep '^geo=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2)
url=$(grep '^wxurl=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2-)
wx=$(grep '^wx=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2- | tr -c '[:print:]\n' '?') # degree byte -> '?'
echo "host ip-api: $host"
echo "kernel geo:  $geo"
echo "kernel url:  $url"
echo "kernel wx:   $wx"
[ -n "$geo" ] || { echo "FAIL: kernel never logged a geo= line (e1000 probe/reset/ARP-to-gateway/DNS never completed)"; exit 1; }
[ "$geo" = "$host" ] || { echo "FAIL: kernel location differs from the host's own ip-api answer"; exit 1; }
lat=${geo%,*}; lon=${geo#*,}
case "$url" in
  *"latitude=$lat&longitude=$lon&"*) ;;
  *) echo "FAIL: Open-Meteo URL does not carry the looked-up lat/lon"; exit 1 ;;
esac
[ -n "$wx" ] || { echo "FAIL: kernel never logged a wx= line (e1000 HTTP fetch did not land)"; exit 1; }
echo "PASS: e1000 (82540EM) carried a real ARP+DNS+TCP+HTTP round trip end to end ($geo, $wx)"
