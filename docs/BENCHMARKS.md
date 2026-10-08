# Benchmarks

Measured by `tools/bench.sh`: the kernel boots headless in QEMU on this Mac,
runs `kernel/bench.h` and prints the numbers over serial. Run it yourself;
the numbers move with the host. Version 2.30.6, 2026-10-08.

| Benchmark | Result |
|---|---|
| boot_to_shell | 240 ms |
| heap_alloc_free | 157 ns/op |
| memcpy | 836 MB/s |
| context_switch | 2831 ns/switch |
| disk_read | 11351 KB/s |

boot_to_shell is timer ticks from the first interrupt to the shell prompt.
heap_alloc_free is one kmalloc plus one kfree, averaged over 20,000.
memcpy moves 256 KB blocks, 16 MB in all. context_switch is one direction
of a round trip to a task that only yields. disk_read is 128 KB of PIO
sectors from a fresh FAT16 image.

## On the real Raspberry Pi 4

Measured on the board on 2026-10-08 by asking Samantha to run the benchmark (the `[[bench]]` action in `arch/arm64/ask.c`). The times are from power-on, on the generic counter.

| Benchmark | Result |
|---|---|
| memcpy | 1109 MB/s |
| heap alloc+free | 30 ns/op |
| desktop up | 0.2 s |
| Wi-Fi joined | 6.4 s |
| clock set from the net | 8.7 s |

memcpy copies a 256 KB block 64 times. alloc is one allocation plus one free, averaged over 20,000. The Wi-Fi time includes loading the chip's firmware and the WPA2 handshake; the clock time adds the DHCP lease and one HTTP request.
