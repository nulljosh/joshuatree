#ifndef SPEAK_H
#define SPEAK_H
/* Text to speech over the network: POSTs {"text":...,"format":"pcm8"} to
   host:port/api/speak (Turing's speech endpoint), which answers with raw
   8-bit unsigned mono PCM at 16000Hz, then plays it on the Sound Blaster.
   Text is cut to SPEAK_TEXT_MAX chars and audio to SPEAK_AUDIO_MAX bytes
   (about 16s). A silent no-op when there is no card, the fetch fails, the
   status is not 200, or the body is too short to be sound. Returns bytes
   played, 0 if nothing played. The body is binary: its length comes from
   the HTTP reply, never from strlen.

   256KB (about 16s) needs the landing demo's 64MB guest (embed.js's
   memory_size); at 32MB, with her face frames loaded, it didn't fit and
   replies were clipped to a 64KB cap (about 4s). */
#define SPEAK_TEXT_MAX  300
#define SPEAK_AUDIO_MAX (256u * 1024u)
#define SPEAK_RATE      16000u

extern const char *speak_voice;   /* "" = Samantha, "joshua" = his clone; set by kernel.c */
unsigned int speak_text(const char *host, unsigned short port, const char *text,
                        unsigned int timeout_ticks);
/* Loudness (mean distance from silence, 0..128) of the clip speak_text is
   playing, over 40ms at elapsed_ticks into it; 0 when nothing plays. For
   sb16's progress hook, so a face can follow her voice. */
unsigned int speak_level(unsigned int elapsed_ticks);
#endif
