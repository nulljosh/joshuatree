/* Fetch-and-play text to speech. See speak.h for the contract. */
#include "speak.h"
#include "http.h"
#include "json.h"
#include "kheap.h"
#include "sb16.h"
#include "serial.h"

static void put_uint(unsigned int v) {
    char d[12]; int n = 0;
    if (v == 0) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + v % 10); v /= 10; }
    char out[12]; int o = 0;
    while (n) out[o++] = d[--n];
    out[o] = 0;
    serial_puts(out);
}

/* The clip playing right now, so Chat can move her mouth with it. */
static const unsigned char *speak_pcm = 0;
static unsigned int speak_len = 0;

unsigned int speak_level(unsigned int elapsed_ticks) {
    if (!speak_pcm) return 0;
    unsigned int at = elapsed_ticks * (SPEAK_RATE / 100u), win = SPEAK_RATE / 25u; /* 40ms */
    if (at >= speak_len) return 0;
    if (at + win > speak_len) win = speak_len - at;
    unsigned int sum = 0;
    for (unsigned int i = 0; i < win; i++) {
        int d = (int)speak_pcm[at + i] - 128;
        sum += (unsigned int)(d < 0 ? -d : d);
    }
    return win ? sum / win : 0;
}

/* Whose voice /api/speak uses: "" is Samantha's default, "joshua" his own clone.
   kernel.c sets it from the command line (portfolio mode is his site). */
const char *speak_voice = "";

unsigned int speak_text(const char *host, unsigned short port, const char *text,
                        unsigned int timeout_ticks) {
    if (!sb16_present() || !text || !text[0]) return 0;

    char clipped[SPEAK_TEXT_MAX + 1];
    unsigned int n = 0;
    while (text[n] && n < SPEAK_TEXT_MAX) { clipped[n] = text[n]; n++; }
    clipped[n] = 0;

    /* Worst case every char escapes to two, plus the fixed JSON frame. */
    static char body[SPEAK_TEXT_MAX * 2 + 64];
    unsigned int b = 0;
    const char *head = "{\"text\":\"";
    while (*head) body[b++] = *head++;
    b += json_escape(clipped, body + b, sizeof(body) - b - 24);
    const char *tail = "\",\"format\":\"pcm8\"";
    while (*tail) body[b++] = *tail++;
    if (speak_voice[0]) { const char *v = ",\"voice\":\""; while (*v) body[b++] = *v++; v = speak_voice; while (*v) body[b++] = *v++; body[b++] = '"'; }
    body[b++] = '}';
    body[b] = 0;

    unsigned char *pcm = kmalloc(SPEAK_AUDIO_MAX);
    if (!pcm) return 0;
    int got = http_post_timeout(host, "/api/speak", port, body, b, pcm, SPEAK_AUDIO_MAX, timeout_ticks);
    int status = http_last_status();
    unsigned int played = 0;
    serial_puts("speak: status="); put_uint((unsigned int)status);
    serial_puts(" bytes="); put_uint(got > 0 ? (unsigned int)got : 0); serial_puts("\n");
    /* A 404 page or a proxy's error body is text, not audio: play only a
       real 200, and only a body long enough to be sound (a few ms). */
    if (status == 200 && got >= 64) {
        speak_pcm = pcm; speak_len = (unsigned int)got;
        if (sb16_play(pcm, (unsigned int)got, SPEAK_RATE)) played = (unsigned int)got;
        speak_pcm = 0; speak_len = 0;
    }
    kfree(pcm);
    return played;
}
