#ifndef SPEAK_H
#define SPEAK_H
/* Text to speech over the network: POSTs {"text":...,"format":"pcm8"} to
   host:port/api/speak (Turing's speech endpoint), which answers with raw
   8-bit unsigned mono PCM at 16000Hz, then plays it on the Sound Blaster.
   Text is cut to SPEAK_TEXT_MAX chars and audio to SPEAK_AUDIO_MAX bytes
   (about 16s). A silent no-op when there is no card, the fetch fails, the
   status is not 200, or the body is too short to be sound. Returns bytes
   played, 0 if nothing played. The body is binary: its length comes from
   the HTTP reply, never from strlen. */
#define SPEAK_TEXT_MAX  300
#define SPEAK_AUDIO_MAX (256u * 1024u)
#define SPEAK_RATE      16000u

unsigned int speak_text(const char *host, unsigned short port, const char *text,
                        unsigned int timeout_ticks);
#endif
