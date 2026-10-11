# Benchmarks

Measured by `tools/bench.sh`: the kernel boots headless in QEMU on this Mac,
runs `kernel/bench.h` and prints the numbers over serial. Run it yourself;
the numbers move with the host. Version 2.54.0, 2026-10-11.

| Benchmark | Result |
|---|---|
| boot_to_shell | 250 ms |
| heap_alloc_free | 165 ns/op |
| memcpy | 803 MB/s |
| context_switch | 2950 ns/switch |
| disk_read | 10640 KB/s |

boot_to_shell is timer ticks from the first interrupt to the shell prompt.
heap_alloc_free is one kmalloc plus one kfree, averaged over 20,000.
memcpy moves 256 KB blocks, 16 MB in all. context_switch is one direction
of a round trip to a task that only yields. disk_read is 128 KB of PIO
sectors from a fresh FAT16 image.
