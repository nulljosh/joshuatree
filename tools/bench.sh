#!/usr/bin/env bash
# Boot the kernel headless with "bench" on the command line, wait for the
# kernel's own "bench done", and print the numbers. With --write, refresh
# docs/BENCHMARKS.md too. Uses disk.img for the disk number when it exists.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f kernel.elf ] || make kernel.elf >/dev/null
LOG=$(mktemp)
IMG=$(mktemp -d)/bench.img
bash tools/mkdisk.sh "$IMG" >/dev/null 2>&1 || IMG=""
DISK=()
[ -n "$IMG" ] && DISK=(-drive "file=$IMG,format=raw,if=ide,index=0")
qemu-system-i386 -kernel kernel.elf -append bench -display none -vga std \
    -serial "file:$LOG" ${DISK[@]+"${DISK[@]}"} >/dev/null 2>&1 &
Q=$!
for _ in $(seq 1 120); do grep -q "^bench done" "$LOG" 2>/dev/null && break; sleep 0.5; done
kill $Q 2>/dev/null || true
grep -q "^bench done" "$LOG" || { echo "bench: kernel never finished (see $LOG)"; exit 1; }
ROWS=$(grep "^bench " "$LOG" | grep -v done | awk '{printf "| %s | %s %s |\n", $2, $3, $4}')
printf "| Benchmark | Result |\n|---|---|\n%s\n" "$ROWS"
if [ "${1:-}" = "--write" ]; then
  {
    echo "# Benchmarks"
    echo
    echo "Measured by \`tools/bench.sh\`: the kernel boots headless in QEMU on this Mac,"
    echo "runs \`kernel/bench.h\` and prints the numbers over serial. Run it yourself;"
    echo "the numbers move with the host. Version $(cat VERSION), $(date +%Y-%m-%d)."
    echo
    printf "| Benchmark | Result |\n|---|---|\n%s\n" "$ROWS"
    echo
    echo "boot_to_shell is timer ticks from the first interrupt to the shell prompt."
    echo "heap_alloc_free is one kmalloc plus one kfree, averaged over 20,000."
    echo "memcpy moves 256 KB blocks, 16 MB in all. context_switch is one direction"
    echo "of a round trip to a task that only yields. disk_read is 128 KB of PIO"
    echo "sectors from a fresh FAT16 image."
  } > docs/BENCHMARKS.md
  echo "wrote docs/BENCHMARKS.md"
fi
rm -f "$LOG"; [ -n "$IMG" ] && rm -rf "$(dirname "$IMG")"
