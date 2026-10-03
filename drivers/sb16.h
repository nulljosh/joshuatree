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

/* Records into out (8-bit unsigned mono PCM) at rate Hz (4000..44100, same
   clamp as sb16_play), up to max_len bytes, blocking one DMA chunk at a
   time (sb16.c's DMA_CHUNK) until either max_len is reached or a chunk's
   IRQ times out. Uses the DSP's old-style single-cycle ADC command
   (0x40 set time constant, 0x24 8-bit input) rather than the SB16-only
   0x42/0xC-series input path: every real SB16 and QEMU's `-device sb16`
   both answer the DSP-2.xx-compatible command set, so this is the one
   record path most likely to work unmodified under both. Returns bytes
   actually captured (0 if there is no card, or the very first chunk times
   out -- a real recording that ran short still returns what it got). */
int sb16_record(unsigned char *out, unsigned int max_len, unsigned int rate);

/* 1.9.26: the async queue behind SYS_AUDIO_PLAY. Same format as sb16_play (8-bit unsigned mono PCM,
   4000..44100 Hz, the Worker's /api/speak clip as is). sb16_queue copies up to len bytes into a
   32KB kernel ring and returns how many it took (0 when full, no card, or sb16_play/record owns
   the card); it never waits. The IRQ chains 1KB DMA transfers out of the ring, so playback needs
   no caller. Playback starts once a full 1KB is queued, or at once when end is nonzero (the last
   piece of a clip). The rate is taken from the call that finds the queue idle; later calls in the
   same clip ignore it. sb16_play and sb16_record return 0 while the queue is busy. */
unsigned int sb16_queue(const unsigned char *pcm, unsigned int len, unsigned int rate, int end);
struct sb16_qstat { unsigned int present, playing, queued, space, rate, played; };
/* played counts bytes (= samples) heard since the queue last went idle, interpolated inside the
   transfer in flight so a 12 fps mouth sees a smooth position. */
void sb16_queue_status(struct sb16_qstat *st);
/* Drops everything not yet in flight; the current 1KB transfer finishes. */
void sb16_queue_stop(void);
/* 1.9.26: async capture behind SYS_AUDIO_RECORD, 8-bit unsigned mono at rate Hz (4000..44100).
   Same DSP-2.xx ADC command as sb16_record, but chained from the IRQ in 4KB transfers into a 32KB
   ring (an overrun drops the oldest). Exclusive with the play queue and sb16_play/record, which
   return 0 while it runs. sb16_rec_start returns 1 armed, 0 busy or no card; it restarts a take
   left on. sb16_rec_read never waits and returns bytes copied (0 when nothing is banked yet; it
   still drains what is left after stop). sb16_rec_stop lets the transfer in flight land, then idles. */
int sb16_rec_start(unsigned int rate);
unsigned int sb16_rec_read(unsigned char *out, unsigned int max_len);
void sb16_rec_stop(void);
int sb16_rec_active(void);
#endif
