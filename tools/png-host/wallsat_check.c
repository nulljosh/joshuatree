/* Ad-hoc host validation for the new kernel/wall_sat.h capture: decodes it
   through the real drivers/png.c and confirms it comes back as exactly
   960x540x3, the same layout wallpaper_rgb uses. Not wired into the
   regression suite (tools/checks/png-host-check.sh already covers
   drivers/png.c itself); this is a one-off sanity check for the new asset. */
#include <stdio.h>
#include <stdlib.h>
#include "png.h"
#include "wall_sat.h"

int main(void) {
    unsigned char *out; unsigned w, h, ch;
    int r = png_decode(wall_sat_png, WALL_SAT_PNG_LEN, &out, &w, &h, &ch);
    if (r) { printf("decode err %d FAIL\n", r); return 1; }
    printf("decoded %ux%u ch=%u %s\n", w, h, ch,
           (w == 960 && h == 540 && ch == 3) ? "ok" : "FAIL");
    int ok = (w == 960 && h == 540 && ch == 3);
    /* dump as a raw PPM for a visual host-side look */
    if (ok) {
        FILE *f = fopen("/tmp/jt-wallsat-decoded.ppm", "wb");
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        fwrite(out, 1, w * h * ch, f);
        fclose(f);
        printf("wrote /tmp/jt-wallsat-decoded.ppm\n");
    }
    free(out);
    return !ok;
}
