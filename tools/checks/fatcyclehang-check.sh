#!/bin/bash
# Regression test for a real bug in drivers/fat.c's dir_get_sector: a
# subdirectory whose FAT cluster chain cycles instead of properly ending
# (exactly what an abnormal shutdown mid-write -- interrupted between
# "allocate the next cluster" and "link the previous one to it" -- can
# leave on disk) used to make ls/cd/any FAT walk into that directory spin
# forever, a real denial-of-service hang, not a crash. See fat.c's
# MAX_DIR_CLUSTER_HOPS comment for the fix.
#
# This builds a real FAT16 disk image, creates a real subdirectory through
# the kernel's own mkdir, then patches the raw image on the host (the
# "abnormal shutdown" this kernel can't produce itself, standing in for
# what one leaves behind) so that directory's own first cluster points to
# itself in the FAT instead of ending, then boots the kernel against that
# corrupted image and proves `ls` inside it still returns within a bounded
# wait, instead of hanging forever. Discriminating: fails (times out, no
# DONE marker) against the pre-fix dir_get_sector, passes against the fix.
set -euo pipefail
cd "$(dirname "$0")/../.."
KERNEL="${JT_KERNEL:-kernel.elf}"
[ "$KERNEL" = kernel.elf ] && make -s kernel.elf

DISK=/tmp/jt-fatcyclehang-test.img
bash tools/mkdisk.sh "$DISK" >/dev/null

WORKDIR=$(mktemp -d /tmp/jt-fatcyclehang-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT

send() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            ".") echo "sendkey dot" ;;
            "-") echo "sendkey minus" ;;
            "/") echo "sendkey slash" ;;
            *)   echo "sendkey $c" ;;
        esac
    done
    echo "sendkey ret"
}

vga_text() {
    python3 - "$1" <<'PYEOF'
import re, sys
bytes_ = []
for l in open(sys.argv[1]).readlines():
    m = re.match(r'^[0-9a-f]{8}:\s+(.*)', l.strip())
    if m: bytes_.extend(int(x, 16) for x in m.group(1).split())
chars = bytes_[0::2]
print(''.join(chr(c) if 32 <= c < 127 else '.' for c in chars))
PYEOF
}

# --- Phase 1: create a real subdirectory ("cyc") on the real disk through
# the kernel's own mkdir, so its root entry and first cluster are exactly
# what fat_mkdir really produces, not hand-forged. ---
LOG1="$WORKDIR/log1"
(
    sleep 3
    echo 'sendkey esc'; sleep 1
    send "mkdir cyc"; sleep 2
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel "$KERNEL" -display none -monitor stdio -serial "file:$WORKDIR/serial1" \
    -drive "file=$DISK,format=raw,if=ide,index=0" > "$LOG1" 2>&1

if ! vga_text "$LOG1" | grep -q "created"; then
    echo "FAIL: setup - mkdir cyc did not report 'created'"
    vga_text "$LOG1"
    exit 1
fi

# --- Phase 2: corrupt the raw image on the host, standing in for what an
# abnormal shutdown mid-write can leave behind: cyc's own first cluster
# gets every directory-entry slot in its whole cluster filled with
# non-terminating (non-zero, non-0xE5) fake entries, so a real directory
# scan never hits an early "end of directory" zero marker naturally, then
# that cluster's own FAT entry is pointed at itself instead of the real
# end-of-chain marker fat_mkdir wrote, a cycle of one. ---
python3 - "$DISK" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path, "r+b") as f:
    boot = bytearray(f.read(512))
    def rd16(o): return boot[o] | (boot[o+1] << 8)
    bytes_per_sector = rd16(11)
    sectors_per_cluster = boot[13]
    reserved_sectors = rd16(14)
    num_fats = boot[16]
    root_entry_count = rd16(17)
    fat_size_sectors = rd16(22)
    assert bytes_per_sector == 512

    fat_start = reserved_sectors
    root_dir_start = fat_start + num_fats * fat_size_sectors
    root_dir_sectors = (root_entry_count * 32 + 511) // 512
    data_start = root_dir_start + root_dir_sectors

    def read_sector(lba):
        f.seek(lba * 512)
        return bytearray(f.read(512))
    def write_sector(lba, data):
        f.seek(lba * 512)
        f.write(bytes(data))

    # Find cyc's root directory entry.
    cluster = None
    for s in range(root_dir_sectors):
        sec = read_sector(root_dir_start + s)
        for i in range(16):
            e = sec[i*32:(i+1)*32]
            if e[:11] == b"CYC        ":
                cluster = e[26] | (e[27] << 8)
        if cluster is not None:
            break
    assert cluster, "cyc directory entry not found on the fresh image"

    def cluster_to_lba(c):
        return data_start + (c - 2) * sectors_per_cluster

    lba = cluster_to_lba(cluster)

    # Fill every sector of cyc's own (only) cluster with fake, valid-looking,
    # never-zero, never-0xE5 directory entries -- keeps a real directory
    # scan walking every entry in this cluster instead of stopping early on
    # the zero-filled tail fat_mkdir actually leaves there.
    fake = bytearray(32)
    fake[0:11] = b"XXXXXXXXTXT"
    fake[11] = 0x20  # attr: plain archive bit, not a directory/volume-id
    for s in range(sectors_per_cluster):
        sec = read_sector(lba + s)
        start = 2 if s == 0 else 0  # sector 0 keeps cyc's real "." and ".." at slots 0/1
        for i in range(start, 16):
            sec[i*32:(i+1)*32] = fake
        write_sector(lba + s, sec)

    # Point this cluster's own FAT entry at itself: a real cycle, the same
    # shape an interrupted "link the previous cluster to the next one"
    # write leaves, instead of the real 0xFFFF end-of-chain marker
    # alloc_cluster put there.
    fat_byte_off = cluster * 2
    fat_row = fat_byte_off // 512
    fatbuf = read_sector(fat_start + fat_row)
    fatbuf[fat_byte_off % 512] = cluster & 0xFF
    fatbuf[fat_byte_off % 512 + 1] = (cluster >> 8) & 0xFF
    for fat_copy in range(num_fats):
        write_sector(fat_start + fat_copy * fat_size_sectors + fat_row, fatbuf)

    print(f"corrupted: cyc cluster={cluster}, sectors_per_cluster={sectors_per_cluster}, FAT[{cluster}]={cluster} (self-loop)")
PYEOF

# --- Phase 3: boot against the corrupted image, cd into cyc, ls it (the
# operation that walks the cycling chain), then a marker command right
# after. A bounded wait: the fix's MAX_DIR_CLUSTER_HOPS=16 cap means a
# real fix finishes in well under this; the pre-fix bug never finishes at
# all, so the marker never appears no matter how long the wait is. ---
LOG2="$WORKDIR/log2"
(
    sleep 3
    echo 'sendkey esc'; sleep 1
    send "cd cyc"; sleep 2
    send "ls"; sleep 12
    send "echo zzzmarkerzzz"; sleep 2
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel "$KERNEL" -display none -monitor stdio -serial "file:$WORKDIR/serial2" \
    -drive "file=$DISK,format=raw,if=ide,index=0" > "$LOG2" 2>&1

TEXT="$(vga_text "$LOG2")"
if echo "$TEXT" | grep -q "zzzmarkerzzz"; then
    echo "PASS: ls returned from a cycling FAT chain instead of hanging"
    exit 0
else
    echo "FAIL: ls never returned (marker command never ran) -- dir_get_sector is spinning on the cyclic chain"
    echo "$TEXT" | tail -20
    exit 1
fi
