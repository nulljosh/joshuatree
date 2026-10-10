/* Brick in the existing ARM app window. All game timing uses the hardware counter. */
#include "brick.h"
static struct brick_game brick;
static int brick_left, brick_right;
static unsigned long brick_last;
static int brick_x(void) { return win_lx + (win_lw - BRICK_W) / 2; }
static int brick_y(void) { return win_ly + 54; }
static void brick_paint(void) {
    int s = (int)window_scale(), x = brick_x(), y = brick_y();
    char score[] = "Bricks 00 / 32    Lives 0";
    score[7] = '0' + brick.score / 10; score[8] = '0' + brick.score % 10;
    score[sizeof score - 2] = '0' + brick.lives;
    cur_hide();
    gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, "Brick");
    gui_text(score, x, win_ly + 32, 0x001C1C1E);
    window_rect(x, y, BRICK_W, BRICK_H, 0x00EFEBE4);
    for (unsigned i = 0; i < 32; i++) if (brick.bricks & (1u << i))
        window_rect(x + 12 + (int)(i % 8) * 54, y + 16 + (int)(i / 8) * 24, 48, 16,
                    i < 16 ? 0x00b5502c : 0x00C88A6D);
    window_rect(x + brick.paddle - BRICK_PW / 2, y + BRICK_PY, BRICK_PW, 7, 0x001C1C1E);
    window_rect(x + brick.x - BRICK_R, y + brick.y - BRICK_R, BRICK_R * 2, BRICK_R * 2, 0x001C1C1E);
    const char *message = brick.state == 2 ? "You cleared the wall. Space to play again."
        : brick.state == 3 ? "Game over. Space to play again."
        : brick.paused ? "Paused. Space or click to resume."
        : !brick.state ? "Space or click to launch." : "";
    if (*message) {
        window_rect(x + 12, y + 125, BRICK_W - 24, 28, 0x00faf8f4);
        gui_text(message, x + (BRICK_W - gui_text_width(message)) / 2, y + 131, 0x001C1C1E);
    }
    gui_text("Arrows / A D or mouse   Space pause   R restart   Esc close", x, y + BRICK_H + 10, 0x0075726E);
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    cur_show();
}
static void brick_open(void) {
    if (brick_live || !con_under) return;
    clock_close(); calc_close(); console_close();
    brick_reset(&brick); brick_left = brick_right = 0; brick_last = 0; brick_live = 1;
    brick_paint(); uart_puts("brick open\n");
}
static void brick_close(void) {
    if (!brick_live) return;
    brick_live = brick_left = brick_right = 0;
    con_live = 1; console_close(); pane_open(cp);
    uart_puts("brick closed\n");
}
static int brick_key(unsigned code, unsigned value) {
    if (!brick_live) return 0;
    if (code == 105 || code == 30) brick_left = value != 0;
    else if (code == 106 || code == 32) brick_right = value != 0;
    else if (value && code == 1) { brick_close(); return 1; }
    else if (value && (code == 57 || code == 28)) { brick_launch(&brick); uart_puts(brick.paused ? "brick paused\n" : "brick play\n"); }
    else if (value && code == 19) { brick_reset(&brick); uart_puts("brick restart\n"); }
    if (brick_live) brick_paint();
    return 1;
}
static void brick_mouse(int lx, int ly) {
    if (spot_live || !brick_live || lx < brick_x() || lx >= brick_x() + BRICK_W || ly < brick_y() || ly >= brick_y() + BRICK_H) return;
    brick_move(&brick, lx - brick_x()); brick_paint();
}
static void brick_tick(void) {
    if (!brick_live) return;
    if (spot_live) { brick_left = brick_right = 0; brick_last = 0; return; }
    unsigned long now, freq;
    __asm__ volatile ("mrs %0, cntpct_el0\n mrs %1, cntfrq_el0" : "=r"(now), "=r"(freq));
    if (now - brick_last < (freq ? freq : 54000000) / 30) return;
    brick_last = now;
    if (brick.paused || brick.state >= 2 || (!brick.state && !brick_left && !brick_right)) return;
    if (brick_left || brick_right) brick_move(&brick, brick.paddle + 7 * (brick_right - brick_left));
    brick_step(&brick); brick_paint();
}
