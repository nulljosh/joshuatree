#ifndef ARM64_AUDIO_H
#define ARM64_AUDIO_H
/* Sound out of the Pi 4's 3.5 mm jack, polled (no DMA). Never called at boot; `beep` in cmd.c is the only caller.
   Both return 0 on success and -1 after printing one `audio:` line; neither can hang. */
int audio_play(const short *samples, int frames, int rate);   /* interleaved stereo, 16-bit, 8000 to 48000 frames a second */
int audio_tone(int freq_hz, int ms);                          /* a triangle wave at 44100 Hz, freq 20 to 20000, ms 1 to 5000 */
#endif
