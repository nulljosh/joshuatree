#!/bin/sh
# Fetches the Pi 4 Bluetooth patch, BCM4345C0.hcd, and its licence into build/bt-fw/ (gitignored), pinned to one
# RPi-Distro/bluez-firmware commit so the bytes never drift under us. Both downloads land in a temp dir and move in
# together, like tools/wifi-fw.sh: a dropped download never leaves a half file behind. The patch is Cypress's,
# redistributable in object code for use with Cypress chips under LICENSE.cypress (saved next to it), and not ours:
# it is never committed. Used by `make -C arch/arm64 bt-fw` (the `pi` target depends on it).
set -eu
D=${1:-build/bt-fw}
C=cdf61dc691a49ff01a124752bd04194907f0f9cd
R=https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/$C
P=debian/firmware/broadcom
# dest name : path at that commit : minimum sane size
SET="BCM4345C0.hcd:$P/BCM4345C0.hcd:60000
LICENSE.cypress:$P/LICENSE.cypress:20000"
ok=1; for s in $SET; do [ -s "$D/${s%%:*}" ] || ok=0; done
[ "$ok" = 1 ] && exit 0
tmp=$(mktemp -d)
for s in $SET; do
    n=${s%%:*}; rest=${s#*:}; src=${rest%:*}; min=${rest##*:}
    curl -sSfL -o "$tmp/$n" "$R/$src"
    sz=$(wc -c < "$tmp/$n")
    if [ "$sz" -lt "$min" ]; then echo "bt-fw: $n is $sz bytes, expected at least $min (wrong path at $C?)" >&2; rm -rf "$tmp"; exit 1; fi
done
rm -rf "$D"; mkdir -p "$(dirname "$D")"; mv "$tmp" "$D"
echo "bt-fw: $(wc -c < "$D/BCM4345C0.hcd") byte patch in $D (RPi-Distro/bluez-firmware @ ${C%${C#???????}})"
