/* Native host harness for drivers/ttf.c, run by tools/checks/ttf-host-check.sh.
   Rasterizes "Ag" at a requested pixel size with the kernel's TTF wrapper
   and dumps each glyph as a raw 8-bit-greyscale PGM so the shell script can
   diff it against PIL's ImageFont rendering of the same TTF file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/ttf.h"

static void write_pgm(const char *path, unsigned char *buf, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(buf, 1, (size_t)(w * h), f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <px_size> <out_A.pgm> <out_g.pgm>\n", argv[0]);
        return 1;
    }
    float px = (float)atof(argv[1]);

    ttf_font_t *font = ttf_load_default();
    if (!font) { fprintf(stderr, "ttf_load_default failed\n"); return 1; }

    const char *chars = "Ag";
    const char *outs[2] = { argv[2], argv[3] };
    for (int i = 0; i < 2; i++) {
        ttf_glyph_t g;
        if (ttf_glyph(font, (unsigned int)chars[i], px, &g) != 0) {
            fprintf(stderr, "ttf_glyph failed for '%c'\n", chars[i]);
            return 1;
        }
        printf("%c: %dx%d xoff=%d yoff=%d advance=%d\n", chars[i], g.width, g.height, g.xoff, g.yoff, g.advance);
        if (g.width > 0 && g.height > 0)
            write_pgm(outs[i], g.coverage, g.width, g.height);
        else
            write_pgm(outs[i], (unsigned char[]){0}, 1, 1);
        ttf_free_glyph(&g);
    }
    ttf_free(font);
    return 0;
}
