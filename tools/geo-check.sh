#!/bin/sh
# v71 (0.65.0): headless, real-network proof that the weather location is
# genuinely dynamic. Boots kernel.elf with QEMU user networking (the same
# rtl8139 + SLIRP setup `make run` uses), waits for gui_run's first weather
# cycle, then reads the two lines weather_fetch mirrors to serial:
#   geo=<lat>,<lon>       what geo_fetch parsed out of ip-api.com's reply
#   wxurl=/v1/forecast?.. the exact Open-Meteo path built from those values
# and compares them against the host's OWN answer from the same service,
# fetched right now by curl. A kernel still carrying a hardcoded constant
# (the pre-v71 49.28/-123.12) fails on the first compare; a kernel that
# parsed the location but forgot to use it fails on the second. Needs
# internet access on the host and python3 (json parse of the curl body).
set -e
cd "$(dirname "$0")/.."
make -s kernel.elf
LOG=/tmp/jt-geo-serial.log
rm -f "$LOG"
host=$(curl -s --max-time 10 http://ip-api.com/json/ | python3 -c 'import json,sys; d=json.load(sys.stdin); print(str(d["lat"])+","+str(d["lon"]))')
[ -n "$host" ] || { echo "FAIL: host could not reach ip-api.com"; exit 1; }
qemu-system-i386 -kernel kernel.elf -display none -vga std \
  -net nic,model=rtl8139 -net user -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 40); do
  sleep 1
  grep -q '^wx=' "$LOG" 2>/dev/null && break
done
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
geo=$(grep '^geo=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2)
url=$(grep '^wxurl=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2-)
wx=$(grep '^wx=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2- | LC_ALL=C tr -c '[:print:]\n' '?') # CP437 degree byte (0xF8) -> '?', so a UTF-8 locale's tr doesn't choke on it
echo "host ip-api: $host"
echo "kernel geo:  $geo"
echo "kernel url:  $url"
echo "kernel wx:   $wx"
[ -n "$geo" ] || { echo "FAIL: kernel never logged a geo= line (no network, or geo_fetch failed)"; exit 1; }
[ "$geo" = "$host" ] || { echo "FAIL: kernel location differs from the host's own ip-api answer"; exit 1; }
lat=${geo%,*}; lon=${geo#*,}
case "$url" in
  *"latitude=$lat&longitude=$lon&"*) ;;
  *) echo "FAIL: Open-Meteo URL does not carry the looked-up lat/lon"; exit 1 ;;
esac
[ -n "$wx" ] || { echo "FAIL: kernel never logged a wx= line (Open-Meteo fetch/parse did not land)"; exit 1; }
echo "PASS: weather location is real and dynamic ($geo), URL built from it, Open-Meteo answered ($wx)"
