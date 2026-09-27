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
    const char *tail = "\",\"format\":\"pcm8\"}";
    while (*tail) body[b++] = *tail++;
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
        if (sb16_play(pcm, (unsigned int)got, SPEAK_RATE)) played = (unsigned int)got;
    }
    kfree(pcm);
    return played;
}
