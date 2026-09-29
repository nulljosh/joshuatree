/* kernel/osk.h -- roadmap 1.9: an on-screen keyboard for boot_to_phone, so a
   phone with no keys can still type into the Notes editor. #included into
   kernel.c just above editor.h (same shape as phone_home.h), so it reads
   boot_to_phone, window_rect, font_draw_string and serial_puts as they are.

   Touch is not a second input stack. QEMU's absolute pointer (and a real
   touchscreen, through the VMware backdoor in drivers/vmmouse.c) already
   reports a tap as "pointer jumps there, left button goes down then up",
   which mouse_click_edge() turns into one click at that point. The editor
   hands that point to osk_tap(); a hit becomes the same make+break
   scancodes IRQ1 would have pushed, through kbd_inject(), so a tapped key
   lands in the very queue a PS/2 key does. Desktop mode never calls in. */
#define OSK_ROW_H 40
#define OSK_ROWS 5
#define OSK_H (OSK_ROW_H * OSK_ROWS)
static const char *const OSK_LETTERS[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm\b"};
static int osk_shown_once;

static int osk_h(void){ return boot_to_phone ? OSK_H : 0; }
static int osk_top(void){ return (int)window_height() - OSK_H; }

/* One rect per key, cell = a tenth of the width. Rows 0-3 are the strings
   above (row 2 indented half a cell, row 3 a whole one, its last cell is
   backspace); row 4 is space, enter, done (done sends Esc, which closes). */
static int osk_key_at(int x, int y, int *rx, int *rw, char *ch){
    int sw = (int)window_width(), row = (y - osk_top()) / OSK_ROW_H;
    if (row < 0 || row >= OSK_ROWS || y < osk_top()) return 0;
    int cell = sw / 10;
    if (row == 4) {
        int a = x < sw * 6 / 10 ? 0 : (x < sw * 8 / 10 ? 1 : 2);
        *rx = a == 0 ? 0 : (a == 1 ? sw * 6 / 10 : sw * 8 / 10);
        *rw = a == 0 ? sw * 6 / 10 : sw * 2 / 10;
        *ch = a == 0 ? ' ' : (a == 1 ? '\n' : 27);
        return 1;
    }
    int off = row == 2 ? cell / 2 : (row == 3 ? cell : 0);
    int col = (x - off) / cell;
    int len = 0; while (OSK_LETTERS[row][len]) len++;
    if (x < off || col < 0 || col >= len) return 0;
    *rx = off + col * cell; *rw = cell; *ch = OSK_LETTERS[row][col];
    return 1;
}

static void osk_draw(void){
    if (!boot_to_phone) return;
    int sw = (int)window_width(), top = osk_top(), cell = sw / 10;
    window_rect(0, top, sw, OSK_H, 0x00E9E4DD);
    for (int row = 0; row < OSK_ROWS; row++) {
        int y = top + row * OSK_ROW_H + 4;
        if (row == 4) {
            static const char *const names[3] = {"space", "enter", "done"};
            int xs[4] = {0, sw * 6 / 10, sw * 8 / 10, sw};
            for (int i = 0; i < 3; i++) {
                window_rect(xs[i] + 3, y, xs[i + 1] - xs[i] - 6, OSK_ROW_H - 8, 0x00FAF8F6);
                font_draw_string(names[i], xs[i] + (xs[i + 1] - xs[i] - font_string_width(names[i])) / 2, y + 10, 0x001C1C1E, -1);
            }
            continue;
        }
        int off = row == 2 ? cell / 2 : (row == 3 ? cell : 0);
        for (int col = 0; OSK_LETTERS[row][col]; col++) {
            char k[2] = {OSK_LETTERS[row][col], 0};
            int x = off + col * cell;
            window_rect(x + 2, y, cell - 4, OSK_ROW_H - 8, 0x00FAF8F6);
            font_draw_string(k[0] == '\b' ? "<" : k, x + (cell - font_string_width(k[0] == '\b' ? "<" : k)) / 2, y + 10, 0x001C1C1E, -1);
        }
    }
    if (!osk_shown_once) { osk_shown_once = 1; serial_puts("osk: shown\n"); }
}

/* Returns 1 if the tap landed on the keyboard (consumed), else 0. */
static int osk_tap(int x, int y){
    int rx, rw; char ch;
    if (!boot_to_phone || !osk_key_at(x, y, &rx, &rw, &ch)) return 0;
    int sc = -1;
    for (int i = 1; i < 58; i++) if (SC[i] == ch && ch != 27) { sc = i; break; }
    if (ch == 27) sc = 0x01;
    if (sc < 0) return 1;
    kbd_inject((unsigned char)sc); kbd_inject((unsigned char)(sc | 0x80));
    serial_puts("osk: key "); serial_puts(ch == ' ' ? "space" : ch == '\n' ? "enter" : ch == '\b' ? "bksp" : ch == 27 ? "done" : (char[]){ch, 0}); serial_puts("\n");
    return 1;
}
