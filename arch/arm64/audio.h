#ifndef ARM64_AUDIO_H
#define ARM64_AUDIO_H
/* Sound out of the Pi 4's 3.5 mm jack. Never called at boot. audio_play and audio_tone are polled (no DMA) and only cmd.c calls
   them (`beep` and the volume keys); both return 0 on success and -1 after printing one `audio:` line, and neither can hang. */
int audio_play(const short *samples, int frames, int rate);   /* interleaved stereo, 16-bit, 8000 to 48000 frames a second */
int audio_tone(int freq_hz, int ms);                          /* a triangle wave at 44100 Hz, freq 20 to 20000, ms 1 to 5000 */
/* Master volume, 0 (silence) to 100, applied as samples enter the ring. Default AUDIO_DEFAULT_VOLUME: change it here when the
   real jack and speaker are tuned. */
#define AUDIO_DEFAULT_VOLUME 70
int audio_volume(void);
void audio_set_volume(int percent);   /* clamped to 0..100; a nonzero level is remembered for audio_mute */
int audio_mute(void);                 /* toggles between 0 and the last nonzero level; returns the new volume */
/* M3e: an app's sound (JT_SYS_AUDIO, answered in main.c, which checks the app's pointers first). Nothing here waits. */
int audio_app_queue(const unsigned char *pcm, unsigned len, unsigned rate, int end);   /* 8-bit unsigned mono: bytes taken, 0 ring full */
void audio_app_status(unsigned st[7]);   /* struct jt_audio_status: version, size, playing, queued, space, rate, played */
void audio_app_stop(void);               /* drop what is queued; the PWM ring plays out */
void audio_pump(void);                   /* the desktop's loop, every pass: the ring into the PWM, the stream's end */
int audio_busy(void);                    /* a stream is queued or running (beep is refused then) */
#endif
