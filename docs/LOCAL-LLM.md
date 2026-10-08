# A local LLM on the Pi

This is a plan for running a small language model on the Raspberry Pi 4 inside Joshua Tree's own kernel, with no Linux and no relay. Today Samantha answers through the relay (`tools/claude-relay/relay.py`). The roadmap item is "Integrated LLM that runs on the box" in `docs/roadmap.md`.

There are two kinds of facts in this doc.

- **Repo facts** come from reading the tree on 2026-10-08. They are not labelled.
- **Model and hardware facts** are marked *(from memory, not checked online)*. I had no web access while writing. Check each one before you build on it.

No speeds are given. Where a speed matters, the answer is: measure on the board.

## Where the ARM build stands today

These are the limits a model has to fit inside.

- **No model code yet.** There is no llm.c in `arch/arm64/`. It would be a new file.
- **The heap is small and never frees.** `kmalloc` in `arch/arm64/main.c` is a 16 MiB bump allocator (`HEAP_SIZE`). `kfree` does nothing. `heap_mark` and `heap_release` roll it back. Only the tiniest checkpoint fits in it. Weights need their own region of RAM, set aside the way the EL0 arena is at 64 MiB.
- **Only about 3 GiB is mapped.** `mmu_init` maps the first 4 GiB as 1 GiB blocks. On a Pi the fourth GiB is device memory, so about 3 GiB of RAM is usable. On an 8 GB board, RAM above 4 GiB cannot be reached until more `l1` entries map it.
- **No SD card driver.** `arch/arm64/wifi.c` drives the Arasan SDHCI (EMMC1), but only to talk to the Wi-Fi chip. Nothing reads files from the SD card yet. *(From memory, not checked online: on a Pi 4 the SD card slot sits on a separate controller, EMMC2.)*
- **One core.** `start.S` parks every core except core 0. The Pi 4 has four Cortex-A72 cores *(from memory, not checked online)*, so three sit idle.
- **Floating point is opt-in per file.** The Makefile builds everything with `-mgeneral-regs-only`. Only `text.c`, `libc.c` and `calc.c` get the FPU. `vectors.S` saves q0 to q31 across interrupts, and `tools/checks/arm64-fp-check.py` proves it. A model file would join that short list.
- **No libm.** A forward pass needs `expf` (softmax, SiLU) and `sqrtf` (RMSNorm). `calc.c` already computes its maths as short series on the FPU, with no libm. Reuse that approach.
- **No GPU compute.** We do not drive the VideoCore for maths. All work runs on the CPU, with NEON SIMD *(from memory, not checked online: the A72 has NEON, and int8 dot products help a lot there)*.

## Which models could run

All sizes below are *(from memory, not checked online)*.

| Class | Example | Rough size | Fit on the Pi |
|---|---|---|---|
| TinyStories toy | Karpathy's llama2.c checkpoints, the smallest (well under 1M params) and the 15M one | Under a megabyte up to about 60 MB as float32 | Easy. The smallest fits in the current heap. It writes short children's stories and nothing else. |
| Larger TinyStories | llama2.c 42M and 110M | Roughly 170 MB and 440 MB as float32 | Fits in RAM. It still writes only stories. Good for timing a real load. |
| Small chat models | Models from about 0.1B to 0.5B params, such as the smallest Qwen or SmolLM sizes | Hundreds of MB at int8, less at 4-bit | Fits. It can follow simple instructions. Expect weak facts and short memory. |
| About 1B chat | TinyLlama 1.1B, Llama 3.2 1B | About 1 GB at int8, about half that at 4-bit | Fits in the mapped 3 GiB. This is about the smallest size that feels like a useful assistant. |
| 3B and up | Llama 3.2 3B, Phi-class | Several GB even at 4-bit | A 4 GB board is too tight. An 8 GB board needs the RAM above 4 GiB mapped first. Likely too slow to be pleasant. Measure before you plan on it. |

llama2.c is the natural start. Its forward pass is one short C file with no dependencies. It reads one flat file of weights in a fixed order, which is easy to carry into a freestanding kernel. It also has an int8 version (runq.c) *(from memory, not checked online)*.

Licences differ per model. Check each one before a release ships weights.

## What quantization costs

All of this is *(from memory, not checked online)*. Judge quality on our own prompts.

- **float32** is the reference. It uses 4 bytes per weight. It is the easiest to get exactly right, and the slowest because it reads the most memory.
- **int8** stores each weight in 1 byte, plus one scale for each small group. Output is usually very close to float32. It cuts memory four times over, and on a memory-bound CPU that cuts time too.
- **4-bit** stores half a byte per weight, plus scales. It loses more than int8. The loss shows most on small models: a 1B model at 4-bit drifts more than a 7B one does. It is often the only way a bigger model fits.
- **Below 4 bits** is for very large models. Skip it here.

A useful habit: keep a float32 build of the same model on the host. Compare token by token before you trust a quantized build.

## Memory needed against the board

This is plain arithmetic. Weights take parameter count times bytes per weight: 4 for float32, 1 for int8, about 0.5 for 4-bit. On top of that:

- **The KV cache** grows with context length. It takes layers × context length × KV width × 2 (keys and values) × bytes per value. Short contexts keep it small.
- **Activations** are a few buffers the width of the model. They are small next to the weights.
- **The rest of the OS** needs its share: the 16 MiB heap, the framebuffer, the kernel image.

How that meets the board:

- **4 GB board.** After the GPU's share and the device GiB, the room left is under 3 GiB *(GPU split from memory, not checked online)*. A 1B model at int8 fits. A 3B model at 4-bit is tight to impossible.
- **8 GB board.** The same 3 GiB until the page tables map more RAM. After that, roughly 7 GB.

## Speed, in words

Generating each token reads every weight once. On a CPU like the A72, reading memory is usually the limit, not the maths *(from memory, not checked online)*. So the rough rules are:

- Fewer bytes per weight means faster tokens. int8 beats float32, and 4-bit beats int8, if unpacking stays cheap.
- Smaller models are faster, roughly in line with their size.
- Reading the prompt can batch many tokens at once. Generating cannot.
- Using all four cores helps the maths. It does not raise the memory ceiling.

The TinyStories toys should feel instant. A 1B model will be slow, and how slow is the question. Measure on the board.

## Loading weights

Simplest first:

1. **Embed them in the kernel image.** `arch/arm64/wifi_fw_gen.sh` already writes an assembly file that pulls the Wi-Fi firmware into `kernel8.img` at build time. A tiny checkpoint can ride along the same way. Nothing new is needed to read it. This only suits small models. *(From memory, not checked online: the Pi firmware may not like a very large `kernel8.img`.)*
2. **Have the firmware load a file.** *(From memory, not checked online: `config.txt` accepts an `initramfs FILE ADDRESS` line. The Pi firmware then copies that file from the SD card's boot partition into RAM before the kernel starts.)* The kernel would find the weights at a fixed address, outside the heap. Add the line to `tools/pi-config.txt`. Verify on the board that it works with `kernel_address=0x80000`, and check how large a file it accepts.
3. **Write a real SD and FAT driver.** Drive EMMC2, read the FAT32 boot partition and stream the file into the weights region. This is the most work. It also opens the card to everything else, such as notes and saved files. On QEMU virt, the virtio disk already reads sector 0, so the reading half can be tested there first.

Whichever path is used, keep a magic number and a length at the front of the weights. Check them before running anything. A cut-short file must give a clear one-line error, not garbage tokens.

## What to measure first

1. **Memory bandwidth and one int8 dot product on the real board.** Time a sum over a large buffer, and one int8 dot product the size of a real layer. Print each as a UART line. These two numbers bound the speed of every model above. Time with the generic timer counter (`cntpct_el0`) or the 100 Hz clock in `arch/arm64/ip.c`. Never wait on `ticks` in `arch/arm64/main.c`. It stops counting early and hangs the boot loop.
2. **The smallest llama2.c checkpoint, embedded, on QEMU.** Run it with greedy sampling, which is deterministic. The output must match the host's output exactly.
3. **The same build on the board.** Time the tokens.

**The check that proves it.** A future arm64-llm-check.py, in the same pattern as `tools/checks/arm64-calc-check.py`. It compiles the same forward pass on the host and runs the smallest checkpoint with a fixed prompt and greedy sampling. Then it boots an `llmtest` build headless on QEMU virt. It asserts that the UART prints exactly the same tokens. Reverting any part of the maths changes the tokens, so the check fails. It goes in `.github/workflows/check.yml`, like the other ARM checks. A model too big to embed can still use this check through the toy model, because the maths is shared.

## A staged plan

1. **Toy, embedded.** Port llama2.c's forward pass as a new arch/arm64/llm.c, with FP on and the `calc.c` series in place of libm. Embed the smallest TinyStories checkpoint. Add a `story` command at the `ask>` row. Prove it with the exact-token check.
2. **Measured.** Run the bandwidth probe and the toy on the board, and write the numbers in `docs/BENCHMARKS.md`. Every later choice of size comes from those numbers.
3. **Room to grow.** Set aside a weights region outside the bump heap. Map RAM above 4 GiB on 8 GB boards. Load a bigger TinyStories model through the firmware path, or a new SD driver.
4. **int8.** Port the int8 path, with NEON dot products. Check it against the float32 host build on a fixed prompt. Expect close output, not identical.
5. **Four cores.** Wake the parked cores and split each matrix multiply across them. Prove it gives the same tokens as one core.
6. **A small chat model.** Choose a chat model of about 0.5B to 1B at int8 or 4-bit, with a tokenizer we can run. Add a chat template. Route an `ask>` question to it when there is no network, or when the user picks local.

## When to still call the relay

The relay stays the default for anything that needs to be right or long.

- Facts, current events and anything a small model would make up.
- Samantha's two read-only file tools for `~/pi-files`, which run on the relay's side.
- The `[[note TEXT]]` and `[[led blink]]` actions that `arch/arm64/ask.c` runs. A small local model will not follow that format reliably. Keep actions on the relay until a check proves otherwise.
- Long answers, code and anything that needs Sonnet or Opus.

The local model is for offline use, privacy, instant toy output and a fallback when the relay cannot be reached. `ask.c` already prints `claude: no network`. That is the natural place to offer the local model.

## Next three, ranked

1. **The exact-token check with an embedded toy.** llm.c, the smallest TinyStories checkpoint in the image, an `llmtest` build, and arm64-llm-check.py comparing host and QEMU tokens.
2. **The bandwidth and timer probe on the real board.** Two UART lines, written into `docs/BENCHMARKS.md`, before any model size is chosen.
3. **A weights region outside the heap, and RAM above 4 GiB.** A fixed region for weights, more `l1` entries for 8 GB boards, and a test of the firmware's `initramfs` load on the card.
