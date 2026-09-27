#ifndef SB16_H
#define SB16_H
/* Sound Blaster 16 driver: the card both QEMU (-device sb16) and the v86
   browser demo emulate. 8-bit unsigned mono PCM through ISA DMA channel 1,
   IRQ 5, base port 0x220. Every port poll is bounded, so a machine without
   the card (real PCs, plain QEMU) sees a silent no-op, never a hang. */

/* Resets the DSP and checks for its 0xAA answer. Returns 1 if a card is
   there (and unmasks IRQ 5), 0 if not. Safe to call more than once. */
int sb16_init(void);

/* 1 once sb16_init found a card. */
int sb16_present(void);

/* Plays len bytes of 8-bit unsigned mono PCM at rate Hz (4000..44100),
   blocking until the last DMA transfer's IRQ lands or times out. Longer
   clips are sent in 64KB chunks. Returns 1 if every chunk completed, 0 if
   there is no card or a transfer timed out. */
int sb16_play(const unsigned char *pcm, unsigned int len, unsigned int rate);

/* Synthesizes a tone of freq Hz for ms milliseconds at 22050Hz and plays
   it. Returns sb16_play's result. The shell's `beep` command calls this. */
int sb16_beep(unsigned int freq, unsigned int ms);

/* Optional callback sb16_play runs over and over while it waits on the
   card (from its own wait loop, never from the IRQ), passing ticks since
   the clip started. Chat uses it to move Samantha's face while she talks.
   0 clears it. */
void sb16_set_progress(void (*fn)(unsigned int elapsed_ticks));

/* Called from irq.c's handler on IRQ 5: acknowledges the DSP. */
void sb16_irq(void);
#endif
