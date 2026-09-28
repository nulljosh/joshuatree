/* The helpers behind app.h: patterns at least two apps repeated by hand,
   pulled out once so each app gets shorter instead of carrying its own copy. */
#include "app.h"
#include "window.h"

void app_begin(const char *title, unsigned int bg){
    window_clear(bg);
    gui_draw_app_titlebar(title);
}

int app_utoa(unsigned int v, char *buf){
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
