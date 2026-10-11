#ifndef ARM64_AUDIO_H
#define ARM64_AUDIO_H
/* Sound out of the Pi 4's 3.5 mm jack, polled (no DMA). Never called at boot; `beep` in cmd.c is the only caller.
   Both return 0 on success and -1 after printing one `audio:` line; neither can hang. */
int audio_play(const short *samples, int frames, int rate);   /* interleaved stereo, 16-bit, 8000 to 48000 frames a second */
int audio_tone(int freq_hz, int ms);                          /* a triangle wave at 44100 Hz, freq 20 to 20000, ms 1 to 5000 */
/* Master volume, 0 (silence) to 100, applied as samples enter the ring. Default AUDIO_DEFAULT_VOLUME: change it here when the
   real jack and speaker are tuned. */
#define AUDIO_DEFAULT_VOLUME 70
int audio_volume(void);
void audio_set_volume(int percent);   /* clamped to 0..100; a nonzero level is remembered for audio_mute */
int audio_mute(void);                 /* toggles between 0 and the last nonzero level; returns the new volume */
#endif
