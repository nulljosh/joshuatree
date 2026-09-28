/* Keyrate, the typing test, in its own translation unit: the pattern for
   moving the rest of the apps out of kernel.c. It needs only the window
   and font drivers, the PIT tick count, and what app.h declares.

   A real typing test needs its own input loop (the read-only page viewer
   closed on the first keystroke), and endless random words, not one fixed
   sentence. No libc here, so a tiny LCG seeded from ticks() picks them:
   good enough for word order, not for anything security-sensitive. */
#include "app.h"
#include "keyrate.h"
#include "window.h"
#include "font.h"
#include "irq.h"

#define BG   0x00FAF8F6
#define INK  0x001C1C1E
#define DONE 0x00884B16 /* typed characters and the wpm readout */

static const char *KEYRATE_WORDS[] = {
    "the","quick","brown","fox","jumps","over","lazy","dog","time","people",
    "water","first","would","these","other","after","words","world","school",
    "still","every","great","might","under","never","found","those","while",
    "place","right","small","sound","between","name","home","read","hand",
    "large","spell","add","even","land","here","must","big","high","such",
    "follow","act","why","ask","men","change","went","light","kind","off",
    "need","house","try","again","animal","point","mother","near","self",
    "work","part","take","get","made","live","where","much","back","only",
};
#define KEYRATE_WORD_COUNT (int)(sizeof(KEYRATE_WORDS) / sizeof(KEYRATE_WORDS[0]))

/* Fills buf with space-separated random words up to cap, returns the length. */
static int keyrate_gen_words(char *buf, int cap, unsigned int *rng) {
    int len = 0;
    while (len < cap - 12) { /* room for a space + the longest word ("between") */
        if (len > 0) buf[len++] = ' ';
        *rng = *rng * 1103515245u + 12345u;
        const char *w = KEYRATE_WORDS[((*rng >> 16) & 0x7fff) % KEYRATE_WORD_COUNT];
        while (*w) buf[len++] = *w++;
    }
    buf[len] = 0;
    return len;
}

void keyrate_open(void){
    app_begin("Keyrate", BG);
    static char target[256];
    unsigned int rng = ticks() | 1; /* |1 so a tick count of 0 never freezes the LCG */
    int tlen = keyrate_gen_words(target, sizeof(target), &rng);
    int pos = 0, started = 0, total_typed = 0;
    unsigned int start_tick = 0;
    int W = (int)window_width(), H = (int)window_height();

    for (;;) {
        /* One clear over everything that can change, every frame, whichever
           branch runs below (the target text and the hint row once overlapped). */
        window_rect(20, 60, W - 40, H - 100, BG);
        window_rect(20, H - 30, W - 40, 16, BG);

        /* ponytail: wraps mid-word, no word-boundary lookahead like monkeytype. */
        for (int i = 0, x = 20, y = 60; i < tlen; i++, x += 8) {
            if (x + 8 > W - 20) { x = 20; y += 16; }
            font_draw_char_mono((unsigned char)target[i], x, y, i < pos ? DONE : INK, -1);
        }

        if (started) {
            unsigned int elapsed = ticks() - start_tick; /* ~100Hz PIT ticks since the first keystroke */
            int wpm = elapsed > 0 ? ((total_typed + pos) * 6000) / (5 * (int)elapsed) : 0; /* (chars/5) / minutes */
            char buf[32]; int n = app_utoa((unsigned int)wpm, buf);
            buf[n++] = ' '; buf[n++] = 'w'; buf[n++] = 'p'; buf[n++] = 'm'; buf[n] = 0;
            font_draw_string(buf, 20, H - 30, DONE, -1);
        } else {
            font_draw_string("type to begin, esc or click to close", 20, H - 30, 0x0075726E, -1);
        }

        int ci = gui_getch_or_click();
        if (ci == -1 || ci == 27) break;
        if (!started) { started = 1; start_tick = ticks(); }
        if ((char)ci == target[pos] && ++pos >= tlen) { /* endless: bank the batch, roll a fresh one, same timer */
            total_typed += tlen;
            tlen = keyrate_gen_words(target, sizeof(target), &rng);
            pos = 0;
        }
    }
}
