#ifndef SPEAK_H
#define SPEAK_H
/* Text to speech over the network: POSTs {"text":...,"format":"pcm8"} to
   host:port/api/speak (Turing's speech endpoint), which answers with raw
   8-bit unsigned mono PCM at 16000Hz, then plays it on the Sound Blaster.
   Text is cut to SPEAK_TEXT_MAX chars and audio to SPEAK_AUDIO_MAX bytes
   (about 4s). A silent no-op when there is no card, the fetch fails, the
   status is not 200, or the body is too short to be sound. Returns bytes
   played, 0 if nothing played. The body is binary: its length comes from
   the HTTP reply, never from strlen.

   SPEAK_AUDIO_MAX was 256KB (about 16s) until the landing demo's own
   check caught it real: this kernel boots the guest with only 32MB of RAM
   (embed.js's memory_size), and by the time Chat has decoded her idle/talk
   face frames (~518KB kept resident) plus everything else already on the
   heap, one contiguous 256KB kmalloc reliably failed -- speak_text
   returned 0 before ever printing "speak: status=" or POSTing anything,
   so Chat looked done "speaking" instantly and stayed silent. 64KB still
   covers a full ordinary reply and actually fits. */
#define SPEAK_TEXT_MAX  300
#define SPEAK_AUDIO_MAX (64u * 1024u)
#define SPEAK_RATE      16000u

unsigned int speak_text(const char *host, unsigned short port, const char *text,
                        unsigned int timeout_ticks);
#endif
